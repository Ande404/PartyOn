// Unit tests for LineFramer: Phase 1 test plan cases TC-F01 to TC-F09.

#include <string>
#include <vector>

#include "LineFramer.hpp"
#include "Protocol.hpp"
#include "TestUtil.hpp"

namespace {

std::vector<LineFramer::Line> feed(LineFramer& framer, const std::string& bytes) {
    return framer.feed(bytes.data(), bytes.size());
}

void oneCommandOneWrite() {  // TC-F01
    LineFramer framer;
    auto lines = feed(framer, "WHO Party1\n");
    CHECK_EQ(lines.size(), 1u);
    CHECK_EQ(lines[0].text, std::string("WHO Party1"));
    CHECK(!lines[0].oversized);
    CHECK_EQ(framer.pendingBytes(), 0u);
}

void byteByByte() {  // TC-F02
    LineFramer framer;
    const std::string command = "JOIN Party1 User1\n";
    std::size_t completed = 0;
    for (std::size_t i = 0; i < command.size(); ++i) {
        auto lines = framer.feed(&command[i], 1);
        if (i + 1 < command.size()) {
            CHECK(lines.empty());
        } else {
            CHECK_EQ(lines.size(), 1u);
            CHECK_EQ(lines[0].text, std::string("JOIN Party1 User1"));
        }
        completed += lines.size();
    }
    CHECK_EQ(completed, 1u);
}

void threeCommandsOneWrite() {  // TC-F03
    LineFramer framer;
    auto lines = feed(framer, "WHO A\nITEMLIST A\nRSVP A Yes\n");
    CHECK_EQ(lines.size(), 3u);
    CHECK_EQ(lines[0].text, std::string("WHO A"));
    CHECK_EQ(lines[1].text, std::string("ITEMLIST A"));
    CHECK_EQ(lines[2].text, std::string("RSVP A Yes"));
}

void splitMidToken() {  // TC-F04
    LineFramer framer;
    CHECK(feed(framer, "CLA").empty());
    CHECK(feed(framer, "IM_ITEM Par").empty());
    auto lines = feed(framer, "ty1 Chips\nWHO");
    CHECK_EQ(lines.size(), 1u);
    CHECK_EQ(lines[0].text, std::string("CLAIM_ITEM Party1 Chips"));
    CHECK_EQ(framer.pendingBytes(), 3u);
}

void utf8SplitAcrossReads() {  // TC-F05
    LineFramer framer;
    // "José" with the two bytes of 'é' (C3 A9) arriving in separate reads
    CHECK(feed(framer, "JOIN Party1 Jos\xC3").empty());
    auto lines = feed(framer, "\xA9\n");
    CHECK_EQ(lines.size(), 1u);
    CHECK_EQ(lines[0].text, std::string("JOIN Party1 Jos\xC3\xA9"));
    CHECK(protocol::isValidUtf8(lines[0].text));
}

void crlfLineEndings() {  // TC-F06
    LineFramer framer;
    auto lines = feed(framer, "WHO Party1\r\nITEMLIST Party1\r\n");
    CHECK_EQ(lines.size(), 2u);
    CHECK_EQ(lines[0].text, std::string("WHO Party1"));
    CHECK_EQ(lines[1].text, std::string("ITEMLIST Party1"));
}

void emptyLines() {  // TC-F07 (team decision: empty line is passed on and answered with 300)
    LineFramer framer;
    auto lines = feed(framer, "\n\r\n");
    CHECK_EQ(lines.size(), 2u);
    CHECK_EQ(lines[0].text, std::string(""));
    CHECK_EQ(lines[1].text, std::string(""));
}

void closedMidLine() {  // TC-F08
    LineFramer framer;
    auto lines = feed(framer, "DELETE_EVENT Party1");
    CHECK(lines.empty());
    CHECK_EQ(framer.pendingBytes(), 19u);
    // The connection closing simply destroys the framer; nothing is executed
}

void oversizedLines() {  // TC-F09
    const std::size_t limit = protocol::kMaxLineBytes;

    {   // exactly at the limit (526 bytes + '\n') is accepted
        LineFramer framer;
        auto lines = feed(framer, std::string(limit - 1, 'A') + "\n");
        CHECK_EQ(lines.size(), 1u);
        CHECK(!lines[0].oversized);
        CHECK_EQ(lines[0].text.size(), limit - 1);
    }
    {   // one byte over is rejected, and the next line still works
        LineFramer framer;
        auto lines = feed(framer, std::string(limit, 'A') + "\nWHO P\n");
        CHECK_EQ(lines.size(), 2u);
        CHECK(lines[0].oversized);
        CHECK_EQ(lines[1].text, std::string("WHO P"));
    }
    {   // oversized with no '\n' yet: reported once, rest discarded across reads
        LineFramer framer;
        CHECK(feed(framer, std::string(300, 'B')).empty());
        auto lines = feed(framer, std::string(300, 'B'));
        CHECK_EQ(lines.size(), 1u);
        CHECK(lines[0].oversized);
        CHECK_EQ(framer.pendingBytes(), 0u);
        CHECK(feed(framer, std::string(1000, 'B')).empty());
        CHECK_EQ(framer.pendingBytes(), 0u);
        lines = feed(framer, "tail\nWHO P\n");
        CHECK_EQ(lines.size(), 1u);
        CHECK_EQ(lines[0].text, std::string("WHO P"));
        CHECK(!lines[0].oversized);
    }
}

}  // namespace

int main() {
    oneCommandOneWrite();
    byteByByte();
    threeCommandsOneWrite();
    splitMidToken();
    utf8SplitAcrossReads();
    crlfLineEndings();
    emptyLines();
    closedMidLine();
    oversizedLines();
    return testutil::report("test_framer");
}
