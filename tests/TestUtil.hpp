#pragma once

// Minimal assertion helpers for the unit tests (no external framework).

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

inline int g_checks = 0;
inline int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond \
                      << std::endl;                                              \
        }                                                                        \
    } while (0)

#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        ++g_checks;                                                                      \
        const auto actualValue = (actual);                                               \
        const auto expectedValue = (expected);                                           \
        if (!(actualValue == expectedValue)) {                                           \
            ++g_failures;                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #actual    \
                      << "\n    actual:   " << testutil::show(actualValue)               \
                      << "\n    expected: " << testutil::show(expectedValue) << std::endl; \
        }                                                                                \
    } while (0)

inline bool startsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

namespace testutil {

template <typename T>
std::string show(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string show(const std::string& value) {
    return "\"" + value + "\"";
}

template <typename T>
std::string show(const std::vector<T>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += (i ? ", " : "") + show(values[i]);
    }
    return out + "]";
}

inline int report(const char* suite) {
    if (g_failures == 0) {
        std::cout << suite << ": all " << g_checks << " checks passed" << std::endl;
        return 0;
    }
    std::cout << suite << ": " << g_failures << " of " << g_checks << " checks FAILED" << std::endl;
    return 1;
}

}  // namespace testutil
