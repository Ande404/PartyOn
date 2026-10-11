#include "Logger.hpp"

#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

namespace logger {

namespace {

std::mutex g_mutex;
std::ofstream g_file;

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

void write(const char* level, const std::string& message) {
    const std::string entry = "[" + timestamp() + "] [" + level + "] " + message;

    std::lock_guard<std::mutex> lock(g_mutex);
    std::cout << entry << std::endl;
    if (g_file.is_open()) {
        g_file << entry << std::endl;
    }
}

}  // namespace

bool init(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_file.open(path, std::ios::app);
    return g_file.is_open();
}

void info(const std::string& message) {
    write("INFO", message);
}

void warn(const std::string& message) {
    write("WARN", message);
}

void error(const std::string& message) {
    write("ERROR", message);
}

std::string escape(const std::string& raw, bool escapeNonAscii) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());

    for (unsigned char c : raw) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20 || c == 0x7F || (escapeNonAscii && c >= 0x80)) {
            out += "\\x";
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

}  // namespace logger
