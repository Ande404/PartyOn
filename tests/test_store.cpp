// Unit tests for EventStore and for AppState saving changes through it.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "AppState.hpp"
#include "Dispatcher.hpp"
#include "EventStore.hpp"
#include "TestUtil.hpp"

namespace fs = std::filesystem;

namespace {

// A fresh, empty directory under the system temp folder, removed afterwards
struct TempDir {
    fs::path path;

    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("partyon-test-" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Event sampleEvent() {
    Event event;
    event.eventCode = "Party1";
    event.organizerName = "Andrew";
    event.date = "10/31/2026";
    event.time = "07:00PM";
    event.guests = {{"Andrew", protocol::Rsvp::Yes, true}, {"Jos\xC3\xA9", protocol::Rsvp::Maybe, false}};
    event.items = {{"Chips", "Andrew"}, {"Soda", ""}};
    return event;
}

bool sameEvent(const Event& a, const Event& b) {
    if (a.eventCode != b.eventCode || a.organizerName != b.organizerName || a.date != b.date || a.time != b.time ||
        a.guests.size() != b.guests.size() || a.items.size() != b.items.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.guests.size(); ++i) {
        if (a.guests[i].userName != b.guests[i].userName || a.guests[i].rsvpStatus != b.guests[i].rsvpStatus ||
            a.guests[i].isOrganizer != b.guests[i].isOrganizer) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (a.items[i].itemName != b.items[i].itemName || a.items[i].claimedBy != b.items[i].claimedBy) {
            return false;
        }
    }
    return true;
}

void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

void serializeRoundTrip() {
    const Event event = sampleEvent();
    std::string error;
    auto parsed = EventStore::parse(EventStore::serialize(event), error);
    CHECK(parsed.has_value());
    CHECK(parsed && sameEvent(*parsed, event));

    // Truncated or tampered files are rejected, never half-loaded
    std::string text = EventStore::serialize(event);
    CHECK(!EventStore::parse(text.substr(0, text.size() - 4), error).has_value());  // no "end"
    CHECK(!EventStore::parse("garbage\n", error).has_value());
    std::string badDate = text;
    badDate.replace(badDate.find("10/31/2026"), 10, "02/30/2026");
    CHECK(!EventStore::parse(badDate, error).has_value());
    std::string badRsvp = text;
    badRsvp.replace(badRsvp.find("Andrew Yes"), 10, "Andrew Purple");
    CHECK(!EventStore::parse(badRsvp, error).has_value());
}

void fileNames() {
    // Case-sensitive codes and codes with '/' get distinct, safe file names
    CHECK_EQ(EventStore::fileNameFor("Party1"), std::string("506172747931.event"));
    CHECK(EventStore::fileNameFor("Party1") != EventStore::fileNameFor("party1"));
    CHECK(EventStore::fileNameFor("a/b").find('/') == std::string::npos);
}

void saveLoadDelete() {
    TempDir dir;
    EventStore store(dir.path);
    std::string error;
    CHECK(store.open(error));

    Event event = sampleEvent();
    CHECK(store.save(event, error));
    event.items[1].claimedBy = "Jos\xC3\xA9";  // overwrite the existing file
    CHECK(store.save(event, error));

    Event other = sampleEvent();
    other.eventCode = "party1";
    CHECK(store.save(other, error));

    std::vector<std::string> warnings;
    std::vector<Event> loaded = store.loadAll(warnings);
    CHECK_EQ(loaded.size(), 2u);
    CHECK(warnings.empty());
    for (const Event& e : loaded) {
        CHECK(sameEvent(e, e.eventCode == "Party1" ? event : other));
    }

    CHECK(store.remove("Party1", error));
    warnings.clear();
    CHECK_EQ(store.loadAll(warnings).size(), 1u);
}

void damagedFilesAreSkipped() {
    TempDir dir;
    EventStore store(dir.path);
    std::string error;
    store.open(error);
    store.save(sampleEvent(), error);

    writeFile(dir.path / "abcd.event", "PARTYON-EVENT 1\ncode X\n");                     // incomplete
    writeFile(dir.path / (EventStore::fileNameFor("Party1") + ".tmp"), "half a save");  // interrupted save
    writeFile(dir.path / "notes.txt", "ignored");

    std::vector<std::string> warnings;
    std::vector<Event> loaded = store.loadAll(warnings);
    CHECK_EQ(loaded.size(), 1u);
    CHECK_EQ(warnings.size(), 2u);
    CHECK(!fs::exists(dir.path / (EventStore::fileNameFor("Party1") + ".tmp")));
}

// Every change made through the server is on disk before its response
void appStateSavesChanges() {
    TempDir dir;
    EventStore store(dir.path);
    std::string error;
    store.open(error);

    {
        AppState state;
        state.setStore(&store);
        Dispatcher dispatcher(state);
        SessionId organizer = state.openSession();
        SessionId guest = state.openSession();
        dispatcher.handle(organizer, "CREATE_EVENT Party1 Andrew 10/31/2026 07:00PM");
        dispatcher.handle(guest, "JOIN Party1 Sam");
        dispatcher.handle(guest, "RSVP Party1 Yes");
        dispatcher.handle(guest, "CLAIM_ITEM Party1 Chips");
        dispatcher.handle(organizer, "CLAIM_ITEM Party1 Soda");
        dispatcher.handle(organizer, "RELEASE_ITEM Party1 Soda");
        dispatcher.handle(organizer, "UPDATE_EVENT Party1 11/01/2026 06:30PM");
        dispatcher.handle(organizer, "CREATE_EVENT Gone Andrew 10/31/2026 07:00PM");
        dispatcher.handle(organizer, "DELETE_EVENT Gone");
    }

    // A "restarted" server sees exactly the same state
    AppState restarted;
    std::vector<std::string> warnings;
    for (const Event& event : store.loadAll(warnings)) {
        CHECK(restarted.restore(event));
    }
    CHECK(!restarted.snapshot("Gone").has_value());
    auto party = restarted.snapshot("Party1");
    CHECK(party.has_value());
    if (party) {
        CHECK_EQ(party->date, std::string("11/01/2026"));
        CHECK_EQ(party->time, std::string("06:30PM"));
        CHECK_EQ(party->guests.size(), 2u);
        CHECK(party->guests[0].isOrganizer);
        CHECK(party->guests[1].rsvpStatus == protocol::Rsvp::Yes);
        CHECK_EQ(party->items.size(), 2u);
        CHECK_EQ(party->items[0].claimedBy, std::string("Sam"));
        CHECK_EQ(party->items[1].claimedBy, std::string(""));
    }

    // Identities are not saved: after a restart everyone must JOIN again
    Dispatcher dispatcher(restarted);
    SessionId again = restarted.openSession();
    CHECK(startsWith(dispatcher.handle(again, "WHO Party1").lines[0], "401"));
    CHECK_EQ(dispatcher.handle(again, "JOIN Party1 Sam").note, std::string("rejoined as existing guest"));
}

// DELETE then CREATE with the same code keeps the new event's file
void deleteThenRecreate() {
    TempDir dir;
    EventStore store(dir.path);
    std::string error;
    store.open(error);

    AppState state;
    state.setStore(&store);
    Dispatcher dispatcher(state);
    SessionId s = state.openSession();
    dispatcher.handle(s, "CREATE_EVENT Reuse Old 10/31/2026 07:00PM");
    dispatcher.handle(s, "DELETE_EVENT Reuse");
    dispatcher.handle(s, "CREATE_EVENT Reuse New 12/01/2026 01:00PM");

    // A CREATE that loses (410) must not overwrite the existing event's file
    SessionId loser = state.openSession();
    CHECK(startsWith(dispatcher.handle(loser, "CREATE_EVENT Reuse Loser 01/01/2027 01:00PM").lines[0], "410"));

    std::vector<std::string> warnings;
    std::vector<Event> loaded = store.loadAll(warnings);
    CHECK_EQ(loaded.size(), 1u);
    CHECK(!loaded.empty() && loaded[0].organizerName == "New");
    CHECK(!loaded.empty() && loaded[0].date == "12/01/2026");
}

}  // namespace

int main() {
    serializeRoundTrip();
    fileNames();
    saveLoadDelete();
    damagedFilesAreSkipped();
    appStateSavesChanges();
    deleteThenRecreate();
    return testutil::report("test_store");
}
