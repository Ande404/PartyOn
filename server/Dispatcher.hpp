#pragma once

// Parses one protocol line, enforces the message format (300 checks happen
// before any state check, per the spec's error precedence), and routes the
// command to the matching AppState handler.

#include <string>

#include "AppState.hpp"

class Dispatcher {
public:
    explicit Dispatcher(AppState& state);

    // `line` is one complete line from the LineFramer (no '\n' or '\r')
    Result handle(SessionId session, const std::string& line);

    // Response for a line the framer rejected as longer than 527 bytes
    static Result oversizedLine();

private:
    AppState& state_;
};
