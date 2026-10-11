#pragma once

// Timestamped server log written to stdout and, after init(), to a file.
// Format: [YYYY-MM-DD HH:MM:SS] [LEVEL] message

#include <string>

namespace logger {

// Opens the log file in append mode; returns false if it cannot be opened
// (logging then continues on stdout only)
bool init(const std::string& path);

void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

// Makes received text safe to print: control bytes become \xNN, and so do
// bytes >= 0x80 when escapeNonAscii is set (used for invalid UTF-8)
std::string escape(const std::string& raw, bool escapeNonAscii = false);

}  // namespace logger
