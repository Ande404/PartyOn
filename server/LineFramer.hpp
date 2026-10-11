#pragma once

// Turns a TCP byte stream into complete protocol lines.
// Implements "Receiver-Side Buffering" from the protocol specification:
//   - only complete lines (ending in '\n') are returned; partial lines are kept
//   - a trailing '\r' is stripped
//   - a line longer than the limit is reported once as oversized, and every
//     byte up to and including its '\n' is discarded, even across later reads
//   - bytes still buffered when the connection closes are simply dropped

#include <cstddef>
#include <string>
#include <vector>

#include "Protocol.hpp"

class LineFramer {
public:
    struct Line {
        std::string text;        // line content without '\n' or trailing '\r'
        bool oversized = false;  // line exceeded the limit; text is empty
    };

    explicit LineFramer(std::size_t maxLineBytes = protocol::kMaxLineBytes);

    // Appends received bytes and returns every line they complete, in order
    std::vector<Line> feed(const char* data, std::size_t length);

    // Bytes held for a line that has not been completed yet
    std::size_t pendingBytes() const { return buffer_.size(); }

private:
    std::size_t maxLineBytes_;
    std::string buffer_;
    bool discarding_ = false;
};
