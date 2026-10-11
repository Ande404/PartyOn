#include "LineFramer.hpp"

LineFramer::LineFramer(std::size_t maxLineBytes) : maxLineBytes_(maxLineBytes) {}

std::vector<LineFramer::Line> LineFramer::feed(const char* data, std::size_t length) {
    std::vector<Line> lines;
    buffer_.append(data, length);

    std::size_t start = 0;
    while (start < buffer_.size()) {
        const std::size_t newline = buffer_.find('\n', start);

        if (discarding_) {
            if (newline == std::string::npos) {
                start = buffer_.size();
                break;
            }
            discarding_ = false;
            start = newline + 1;
            continue;
        }

        if (newline == std::string::npos) {
            // No '\n' yet: keep waiting unless the line is already too long
            if (buffer_.size() - start > maxLineBytes_) {
                lines.push_back({"", true});
                discarding_ = true;
                start = buffer_.size();
            }
            break;
        }

        // The limit counts the terminating '\n'
        const std::size_t lineBytes = newline - start + 1;
        if (lineBytes > maxLineBytes_) {
            lines.push_back({"", true});
        } else {
            std::string text = buffer_.substr(start, newline - start);
            if (!text.empty() && text.back() == '\r') {
                text.pop_back();
            }
            lines.push_back({std::move(text), false});
        }
        start = newline + 1;
    }

    buffer_.erase(0, start);
    return lines;
}
