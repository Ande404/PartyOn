#include "Dispatcher.hpp"

#include <map>
#include <vector>

using namespace protocol;

namespace {

enum class ArgKind { Field, Date, Time, Status };

// Expected arguments for every command the server accepts (HELP is client-side only)
const std::map<std::string, std::vector<ArgKind>> kCommands = {
    {"JOIN", {ArgKind::Field, ArgKind::Field}},
    {"CREATE_EVENT", {ArgKind::Field, ArgKind::Field, ArgKind::Date, ArgKind::Time}},
    {"DELETE_EVENT", {ArgKind::Field}},
    {"RSVP", {ArgKind::Field, ArgKind::Status}},
    {"ITEMLIST", {ArgKind::Field}},
    {"CLAIM_ITEM", {ArgKind::Field, ArgKind::Field}},
    {"RELEASE_ITEM", {ArgKind::Field, ArgKind::Field}},
    {"UPDATE_EVENT", {ArgKind::Field, ArgKind::Date, ArgKind::Time}},
    {"WHO", {ArgKind::Field}},
    {"QUIT", {}},
};

Result formatError(const std::string& reason) {
    return Result::single(response::kFormatError, reason);
}

bool isValidArg(ArgKind kind, const std::string& value) {
    Rsvp unused;
    switch (kind) {
        case ArgKind::Field: return isValidField(value);
        case ArgKind::Date: return isValidDate(value);
        case ArgKind::Time: return isValidTime(value);
        case ArgKind::Status: return parseRsvp(value, unused);
    }
    return false;
}

const char* describe(ArgKind kind) {
    switch (kind) {
        case ArgKind::Field: return "field";
        case ArgKind::Date: return "date (expected MM/DD/YYYY)";
        case ArgKind::Time: return "time (expected HH:MMAM or HH:MMPM)";
        case ArgKind::Status: return "RSVP status (expected Yes, No, or Maybe)";
    }
    return "field";
}

}  // namespace

Dispatcher::Dispatcher(AppState& state) : state_(state) {}

Result Dispatcher::oversizedLine() {
    return formatError("line longer than 527 bytes");
}

Result Dispatcher::handle(SessionId session, const std::string& line) {
    if (!isValidUtf8(line)) {
        return formatError("not valid UTF-8");
    }

    const std::vector<std::string> fields = splitFields(line);
    const std::string& command = fields[0];

    auto spec = kCommands.find(command);
    if (spec == kCommands.end()) {
        return formatError(command.empty() ? "empty command" : "unknown command");
    }

    const std::vector<ArgKind>& expected = spec->second;
    if (fields.size() - 1 != expected.size()) {
        return formatError(command + " takes " + std::to_string(expected.size()) + " field(s), got " +
                           std::to_string(fields.size() - 1));
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!isValidArg(expected[i], fields[i + 1])) {
            return formatError(std::string("invalid ") + describe(expected[i]) + " in position " +
                               std::to_string(i + 1));
        }
    }

    if (command == "JOIN") {
        return state_.join(session, fields[1], fields[2]);
    }
    if (command == "CREATE_EVENT") {
        return state_.createEvent(session, fields[1], fields[2], fields[3], fields[4]);
    }
    if (command == "DELETE_EVENT") {
        return state_.deleteEvent(session, fields[1]);
    }
    if (command == "RSVP") {
        Rsvp status = Rsvp::Maybe;
        parseRsvp(fields[2], status);
        return state_.rsvp(session, fields[1], status);
    }
    if (command == "ITEMLIST") {
        return state_.itemList(session, fields[1]);
    }
    if (command == "CLAIM_ITEM") {
        return state_.claimItem(session, fields[1], fields[2]);
    }
    if (command == "RELEASE_ITEM") {
        return state_.releaseItem(session, fields[1], fields[2]);
    }
    if (command == "UPDATE_EVENT") {
        return state_.updateEvent(session, fields[1], fields[2], fields[3]);
    }
    if (command == "WHO") {
        return state_.who(session, fields[1]);
    }

    // QUIT: no response; the connection is closed by the caller
    Result result;
    result.closeConnection = true;
    result.note = "client sent QUIT";
    return result;
}
