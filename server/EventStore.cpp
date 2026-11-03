#include "EventStore.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

using namespace protocol;

namespace fs = std::filesystem;

namespace {

const char* kHeader = "PARTYON-EVENT 1";
const char* kExtension = ".event";
const char* kTempSuffix = ".tmp";

bool readFile(const fs::path& path, std::string& contents) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    contents = buffer.str();
    return !in.bad();
}

}  // namespace

EventStore::EventStore(fs::path directory) : directory_(std::move(directory)) {}

bool EventStore::open(std::string& error) {
    std::error_code ec;
    fs::create_directories(directory_, ec);
    if (ec || !fs::is_directory(directory_, ec)) {
        error = "cannot use data directory " + directory_.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string EventStore::fileNameFor(const std::string& eventCode) {
    static const char kHex[] = "0123456789abcdef";
    std::string name;
    for (unsigned char c : eventCode) {
        name += kHex[c >> 4];
        name += kHex[c & 0x0F];
    }
    return name + kExtension;
}

std::string EventStore::serialize(const Event& event) {
    std::string text = std::string(kHeader) + "\n";
    text += "code " + event.eventCode + "\n";
    text += "organizer " + event.organizerName + "\n";
    text += "date " + event.date + "\n";
    text += "time " + event.time + "\n";
    for (const Guest& guest : event.guests) {
        text += "guest " + guest.userName + " " + toString(guest.rsvpStatus) + "\n";
    }
    for (const Item& item : event.items) {
        text += "item " + item.itemName + (item.claimedBy.empty() ? "" : " " + item.claimedBy) + "\n";
    }
    return text + "end\n";
}

std::optional<Event> EventStore::parse(const std::string& text, std::string& error) {
    std::istringstream in(text);
    std::string line;
    Event event;
    bool sawHeader = false;
    bool sawEnd = false;

    while (std::getline(in, line)) {
        if (!sawHeader) {
            if (line != kHeader) {
                error = "missing header";
                return std::nullopt;
            }
            sawHeader = true;
            continue;
        }
        if (line == "end") {
            sawEnd = true;
            break;
        }

        const std::vector<std::string> fields = splitFields(line);
        const std::string& key = fields[0];
        auto valid = [&](std::size_t count) {
            if (fields.size() != count) {
                return false;
            }
            for (std::size_t i = 1; i < fields.size(); ++i) {
                if (!isValidField(fields[i])) {
                    return false;
                }
            }
            return true;
        };

        if (key == "code" && valid(2)) {
            event.eventCode = fields[1];
        } else if (key == "organizer" && valid(2)) {
            event.organizerName = fields[1];
        } else if (key == "date" && valid(2) && isValidDate(fields[1])) {
            event.date = fields[1];
        } else if (key == "time" && valid(2) && isValidTime(fields[1])) {
            event.time = fields[1];
        } else if (key == "guest" && valid(3)) {
            Guest guest;
            guest.userName = fields[1];
            if (!parseRsvp(fields[2], guest.rsvpStatus) || event.findGuest(guest.userName) != nullptr) {
                error = "bad guest line: " + line;
                return std::nullopt;
            }
            event.guests.push_back(guest);
        } else if (key == "item" && (valid(2) || valid(3))) {
            if (event.findItem(fields[1]) != nullptr) {
                error = "duplicate item: " + fields[1];
                return std::nullopt;
            }
            event.items.push_back(Item{fields[1], fields.size() == 3 ? fields[2] : ""});
        } else {
            error = "bad line: " + line;
            return std::nullopt;
        }
    }

    if (!sawEnd) {
        error = "file is incomplete (no end line)";
        return std::nullopt;
    }
    if (event.eventCode.empty() || event.organizerName.empty() || event.date.empty() || event.time.empty()) {
        error = "missing code, organizer, date, or time";
        return std::nullopt;
    }

    Guest* organizer = event.findGuest(event.organizerName);
    if (organizer == nullptr) {
        error = "organizer is not a guest";
        return std::nullopt;
    }
    organizer->isOrganizer = true;
    return event;
}

std::vector<Event> EventStore::loadAll(std::vector<std::string>& warnings) const {
    std::vector<Event> events;
    std::error_code ec;

    for (const fs::directory_entry& file : fs::directory_iterator(directory_, ec)) {
        const fs::path path = file.path();
        const std::string name = path.filename().string();

        if (path.extension() == kTempSuffix) {
            // A save that was interrupted before its rename; the old file is intact
            fs::remove(path, ec);
            warnings.push_back("removed unfinished save " + name);
            continue;
        }
        if (path.extension() != kExtension) {
            continue;
        }

        std::string contents;
        std::string error;
        if (!readFile(path, contents)) {
            warnings.push_back("cannot read " + name);
            continue;
        }
        std::optional<Event> event = parse(contents, error);
        if (!event) {
            warnings.push_back("skipped " + name + ": " + error);
            continue;
        }
        if (fileNameFor(event->eventCode) != name) {
            warnings.push_back("skipped " + name + ": file name does not match event code");
            continue;
        }
        events.push_back(std::move(*event));
    }

    if (ec) {
        warnings.push_back("cannot list " + directory_.string() + ": " + ec.message());
    }
    return events;
}

bool EventStore::save(const Event& event, std::string& error) const {
    const fs::path target = directory_ / fileNameFor(event.eventCode);
    fs::path temp = target;
    temp += kTempSuffix;

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << serialize(event);
        out.flush();
        if (!out) {
            error = "cannot write " + temp.string();
            return false;
        }
    }

    std::error_code ec;
    fs::rename(temp, target, ec);  // replaces the old file in one step
    if (ec) {
        error = "cannot replace " + target.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool EventStore::remove(const std::string& eventCode, std::string& error) const {
    std::error_code ec;
    fs::remove(directory_ / fileNameFor(eventCode), ec);
    if (ec) {
        error = "cannot delete file for " + eventCode + ": " + ec.message();
        return false;
    }
    return true;
}
