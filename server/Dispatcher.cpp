#include "Dispatcher.hpp"

#include <map>
#include <vector>

using namespace protocol;

namespace {

enum class ArgKind { EventCode, UserName, ItemName, Date, Time, Status };

struct CommandSpec {
    std::vector<ArgKind> args;
    const char* usage;  // shown to the user when the field count is wrong
};

// Every command the server accepts (HELP is client-side only)
const std::map<std::string, CommandSpec> kCommands = {
    {"JOIN", {{ArgKind::EventCode, ArgKind::UserName}, "JOIN <EventCode> <UserName>"}},
    {"CREATE_EVENT",
     {{ArgKind::EventCode, ArgKind::UserName, ArgKind::Date, ArgKind::Time},
      "CREATE_EVENT <EventCode> <UserName> <MM/DD/YYYY> <HH:MMAM|PM>"}},
    {"DELETE_EVENT", {{ArgKind::EventCode}, "DELETE_EVENT <EventCode>"}},
    {"RSVP", {{ArgKind::EventCode, ArgKind::Status}, "RSVP <EventCode> <Yes|No|Maybe>"}},
    {"ITEMLIST", {{ArgKind::EventCode}, "ITEMLIST <EventCode>"}},
    {"CLAIM_ITEM", {{ArgKind::EventCode, ArgKind::ItemName}, "CLAIM_ITEM <EventCode> <ItemName>"}},
    {"RELEASE_ITEM", {{ArgKind::EventCode, ArgKind::ItemName}, "RELEASE_ITEM <EventCode> <ItemName>"}},
    {"UPDATE_EVENT",
     {{ArgKind::EventCode, ArgKind::Date, ArgKind::Time}, "UPDATE_EVENT <EventCode> <MM/DD/YYYY> <HH:MMAM|PM>"}},
    {"WHO", {{ArgKind::EventCode}, "WHO <EventCode>"}},
    {"QUIT", {{}, "QUIT"}},
};

Result formatError(const std::string& reason) {
    return Result::error(response::kFormatError, reason);
}

bool isValidArg(ArgKind kind, const std::string& value) {
    Rsvp unused;
    switch (kind) {
        case ArgKind::EventCode:
        case ArgKind::UserName:
        case ArgKind::ItemName: return isValidField(value);
        case ArgKind::Date: return isValidDate(value);
        case ArgKind::Time: return isValidTime(value);
        case ArgKind::Status: return parseRsvp(value, unused);
    }
    return false;
}

// "invalid <name> in field N (<hint>)"
const char* argName(ArgKind kind) {
    switch (kind) {
        case ArgKind::EventCode: return "event code";
        case ArgKind::UserName: return "username";
        case ArgKind::ItemName: return "item name";
        case ArgKind::Date: return "date";
        case ArgKind::Time: return "time";
        case ArgKind::Status: return "RSVP status";
    }
    return "field";
}

const char* argHint(ArgKind kind) {
    switch (kind) {
        case ArgKind::EventCode:
        case ArgKind::UserName:
        case ArgKind::ItemName: return "1-100 bytes, no spaces";
        case ArgKind::Date: return "expected a real date as MM/DD/YYYY";
        case ArgKind::Time: return "expected HH:MMAM or HH:MMPM";
        case ArgKind::Status: return "expected Yes, No, or Maybe";
    }
    return "";
}

}  // namespace

Dispatcher::Dispatcher(AppState& state) : state_(state) {}

Result Dispatcher::oversizedLine() {
    return formatError("line longer than 527 bytes");
}

Result Dispatcher::handle(SessionId session, const std::string& line) {
    if (!isValidUtf8(line)) {
        return formatError("message is not valid UTF-8");
    }

    if (line.empty()) {
        return formatError("empty command");
    }

    const std::vector<std::string> fields = splitFields(line);
    const std::string& command = fields[0];

    // A leading, trailing, or doubled space shows up as an empty field
    for (const std::string& field : fields) {
        if (field.empty()) {
            return formatError("empty field (extra space?)");
        }
    }

    auto spec = kCommands.find(command);
    if (spec == kCommands.end()) {
        return formatError("unknown command (commands are uppercase, e.g. JOIN; type HELP for the list)");
    }

    const std::vector<ArgKind>& expected = spec->second.args;
    if (fields.size() - 1 != expected.size()) {
        return formatError("wrong number of fields (got " + std::to_string(fields.size() - 1) +
                           "), usage: " + spec->second.usage);
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!isValidArg(expected[i], fields[i + 1])) {
            return formatError(std::string("invalid ") + argName(expected[i]) + " in field " +
                               std::to_string(i + 1) + " (" + argHint(expected[i]) + ")");
        }
    }

    Result result = route(session, fields);
    result.command = command;
    if (fields.size() > 1) {
        result.eventCode = fields[1];
    }
    return result;
}

Result Dispatcher::route(SessionId session, const std::vector<std::string>& fields) {
    const std::string& command = fields[0];

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
