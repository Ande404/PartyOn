// Concurrency tests for AppState: many threads applying commands at once.
// Build with `make tsan` to also check for data races.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AppState.hpp"
#include "Dispatcher.hpp"
#include "TestUtil.hpp"

namespace {

using Lines = std::vector<std::string>;

// Releases all waiting threads at once, so their commands really overlap
class StartGate {
public:
    explicit StartGate(int parties) : remaining_(parties) {}

    void arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (--remaining_ == 0) {
            open_.notify_all();
            return;
        }
        open_.wait(lock, [this] { return remaining_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable open_;
    int remaining_;
};

// Records every line the server would send, per session, in delivery order
struct Recorder {
    std::mutex mutex;
    std::map<SessionId, Lines> sent;

    void attach(AppState& state) {
        state.setSink([this](SessionId session, const std::string& block) {
            std::lock_guard<std::mutex> lock(mutex);
            std::size_t start = 0;
            for (std::size_t nl = block.find('\n'); nl != std::string::npos; nl = block.find('\n', start)) {
                sent[session].push_back(block.substr(start, nl - start));
                start = nl + 1;
            }
        });
    }

    Lines linesFor(SessionId session) {
        std::lock_guard<std::mutex> lock(mutex);
        return sent[session];
    }
};

std::string status(const Result& result) {
    return result.lines.empty() ? "" : result.lines.front().substr(0, 3);
}

void runThreads(int count, const std::function<void(int)>& body) {
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i) {
        threads.emplace_back(body, i);
    }
    for (std::thread& t : threads) {
        t.join();
    }
}

// TC-C01 / TC-C02 / conflict row 1: many clients claim the same item
void sameItemRace() {
    constexpr int kClients = 16;
    constexpr int kRounds = 25;

    AppState state;
    Dispatcher dispatcher(state);
    std::vector<SessionId> sessions;
    SessionId organizer = state.openSession();
    dispatcher.handle(organizer, "CREATE_EVENT Race Andrew 10/31/2026 07:00PM");
    for (int i = 0; i < kClients; ++i) {
        sessions.push_back(state.openSession());
        dispatcher.handle(sessions.back(), "JOIN Race Guest" + std::to_string(i));
    }

    for (int round = 0; round < kRounds; ++round) {
        const std::string item = "Cake" + std::to_string(round);
        StartGate gate(kClients);
        std::atomic<int> ok{0};
        std::atomic<int> conflict{0};
        runThreads(kClients, [&](int i) {
            gate.arriveAndWait();
            const std::string code = status(dispatcher.handle(sessions[i], "CLAIM_ITEM Race " + item));
            (code == "200" ? ok : conflict)++;
        });
        CHECK_EQ(ok.load(), 1);
        CHECK_EQ(conflict.load(), kClients - 1);
    }

    // Every item ended up with exactly one claimer
    auto event = state.snapshot("Race");
    CHECK(event.has_value());
    CHECK_EQ(event->items.size(), static_cast<std::size_t>(kRounds));
    for (const Item& item : event->items) {
        CHECK(!item.claimedBy.empty());
    }
}

// TC-C04 / conflict row 4: many clients create the same event code
void sameCodeCreateRace() {
    constexpr int kClients = 16;
    AppState state;
    Dispatcher dispatcher(state);
    std::vector<SessionId> sessions;
    for (int i = 0; i < kClients; ++i) {
        sessions.push_back(state.openSession());
    }

    StartGate gate(kClients);
    std::atomic<int> created{0};
    std::atomic<int> conflict{0};
    runThreads(kClients, [&](int i) {
        gate.arriveAndWait();
        const std::string code =
            status(dispatcher.handle(sessions[i], "CREATE_EVENT Same User" + std::to_string(i) + " 10/31/2026 07:00PM"));
        (code == "201" ? created : conflict)++;
    });
    CHECK_EQ(created.load(), 1);
    CHECK_EQ(conflict.load(), kClients - 1);
    CHECK_EQ(state.snapshot("Same")->guests.size(), 1u);
}

// TC-C06 / conflict row 3: different events are independent and all succeed
void differentEventsInParallel() {
    constexpr int kEvents = 8;
    constexpr int kItems = 200;
    AppState state;
    Dispatcher dispatcher(state);

    StartGate gate(kEvents);
    std::atomic<int> failures{0};
    runThreads(kEvents, [&](int i) {
        const SessionId session = state.openSession();
        const std::string code = "Party" + std::to_string(i);
        dispatcher.handle(session, "CREATE_EVENT " + code + " Host 10/31/2026 07:00PM");
        gate.arriveAndWait();
        for (int item = 0; item < kItems; ++item) {
            if (status(dispatcher.handle(session, "CLAIM_ITEM " + code + " Item" + std::to_string(item))) != "200") {
                ++failures;
            }
        }
    });
    CHECK_EQ(failures.load(), 0);
    for (int i = 0; i < kEvents; ++i) {
        CHECK_EQ(state.snapshot("Party" + std::to_string(i))->items.size(), static_cast<std::size_t>(kItems));
    }
}

// Notifications reach every connection in the same order the changes were applied
void notificationOrder() {
    constexpr int kClients = 8;
    constexpr int kClaimsEach = 50;
    AppState state;
    Recorder recorder;
    recorder.attach(state);
    Dispatcher dispatcher(state);

    SessionId organizer = state.openSession();
    dispatcher.handle(organizer, "CREATE_EVENT Order Host 10/31/2026 07:00PM");
    std::vector<SessionId> sessions;
    for (int i = 0; i < kClients; ++i) {
        sessions.push_back(state.openSession());
        dispatcher.handle(sessions.back(), "JOIN Order G" + std::to_string(i));
    }

    StartGate gate(kClients);
    runThreads(kClients, [&](int i) {
        gate.arriveAndWait();
        for (int n = 0; n < kClaimsEach; ++n) {
            dispatcher.handle(sessions[i], "CLAIM_ITEM Order G" + std::to_string(i) + "_" + std::to_string(n));
        }
    });

    auto notificationsOnly = [](const Lines& lines) {
        Lines result;
        for (const std::string& line : lines) {
            if (startsWith(line, "210 Order ITEM_CLAIMED")) {
                result.push_back(line);
            }
        }
        return result;
    };

    // The organizer saw every claim; each guest saw every claim made after it joined
    const Lines reference = notificationsOnly(recorder.linesFor(organizer));
    CHECK_EQ(reference.size(), static_cast<std::size_t>(kClients * kClaimsEach));
    for (SessionId session : sessions) {
        CHECK_EQ(notificationsOnly(recorder.linesFor(session)), reference);
    }

    // Each requester got its own 200 OK before the notification its claim caused
    const Lines mine = recorder.linesFor(sessions[0]);
    const std::string firstClaim = "210 Order ITEM_CLAIMED G0_0 claimed by G0";
    std::size_t okIndex = mine.size();
    std::size_t notifyIndex = mine.size();
    for (std::size_t i = 0; i < mine.size(); ++i) {
        if (mine[i] == firstClaim) {
            notifyIndex = i;
            break;
        }
        if (mine[i] == "200 OK") {
            okIndex = i;  // the last 200 OK before our own notification
        }
    }
    CHECK(okIndex < notifyIndex);
}

// Conflict rows 7 / 7b: DELETE_EVENT races with guests' commands
void deleteRacesWithClaims() {
    constexpr int kGuests = 8;
    constexpr int kRounds = 20;

    for (int round = 0; round < kRounds; ++round) {
        AppState state;
        Recorder recorder;
        recorder.attach(state);
        Dispatcher dispatcher(state);

        SessionId organizer = state.openSession();
        dispatcher.handle(organizer, "CREATE_EVENT Doomed Host 10/31/2026 07:00PM");
        std::vector<SessionId> guests;
        for (int i = 0; i < kGuests; ++i) {
            guests.push_back(state.openSession());
            dispatcher.handle(guests.back(), "JOIN Doomed G" + std::to_string(i));
        }

        StartGate gate(kGuests + 1);
        std::vector<std::string> codes(kGuests);
        std::string deleteCode;
        runThreads(kGuests + 1, [&](int i) {
            gate.arriveAndWait();
            if (i == kGuests) {
                deleteCode = status(dispatcher.handle(organizer, "DELETE_EVENT Doomed"));
            } else {
                codes[i] = status(dispatcher.handle(guests[i], "CLAIM_ITEM Doomed Item" + std::to_string(i)));
            }
        });

        CHECK_EQ(deleteCode, std::string("202"));
        CHECK(!state.snapshot("Doomed").has_value());
        for (int i = 0; i < kGuests; ++i) {
            // Applied before the delete: 200 and then EVENT_DELETED; after: 401
            CHECK(codes[i] == "200" || codes[i] == "401");
            const Lines lines = recorder.linesFor(guests[i]);
            const bool gotDeleted = !lines.empty() &&
                                    lines.back() == "210 Doomed EVENT_DELETED This event has been deleted";
            if (codes[i] == "200") {
                CHECK(gotDeleted);
            }
        }
    }
}

// Sessions joining, changing RSVP, and leaving while others claim and release
void sessionChurn() {
    constexpr int kChurners = 6;
    constexpr int kIterations = 200;
    AppState state;
    Recorder recorder;
    recorder.attach(state);
    Dispatcher dispatcher(state);

    SessionId organizer = state.openSession();
    dispatcher.handle(organizer, "CREATE_EVENT Busy Host 10/31/2026 07:00PM");

    std::atomic<bool> done{false};
    std::thread claimer([&] {
        SessionId session = state.openSession();
        dispatcher.handle(session, "JOIN Busy Claimer");
        while (!done) {
            dispatcher.handle(session, "CLAIM_ITEM Busy Chips");
            dispatcher.handle(session, "RELEASE_ITEM Busy Chips");
        }
        state.closeSession(session);
    });

    std::atomic<int> badCloses{0};  // CHECK is not thread-safe, so count here
    runThreads(kChurners, [&](int i) {
        for (int n = 0; n < kIterations; ++n) {
            SessionId session = state.openSession();
            dispatcher.handle(session, "JOIN Busy Churn" + std::to_string(i));
            dispatcher.handle(session, std::string("RSVP Busy ") + (n % 2 ? "Yes" : "No"));
            dispatcher.handle(session, "WHO Busy");
            if (state.closeSession(session) != 1u) {
                ++badCloses;
            }
        }
    });
    done = true;
    claimer.join();
    CHECK_EQ(badCloses.load(), 0);

    // Guests persist after their connections close; only the organizer is still connected
    auto event = state.snapshot("Busy");
    CHECK_EQ(event->guests.size(), static_cast<std::size_t>(kChurners + 2));
    Result after = dispatcher.handle(organizer, "CLAIM_ITEM Busy Cake");
    CHECK_EQ(after.notifications.size(), 1u);
}

}  // namespace

int main() {
    sameItemRace();
    sameCodeCreateRace();
    differentEventsInParallel();
    notificationOrder();
    deleteRacesWithClaims();
    sessionChurn();
    return testutil::report("test_concurrency");
}
