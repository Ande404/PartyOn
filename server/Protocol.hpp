#pragma once

// PartyOn v1.0 wire-format constants, field validation, and message text.
// See docs/UpdatedProtocolSpecification.md.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace protocol {

constexpr std::size_t kMaxLineBytes = 527;   // includes the terminating '\n'
constexpr std::size_t kMaxFieldBytes = 100;  // EventCode, UserName, ItemName
constexpr std::uint16_t kDefaultPort = 5527;

enum class Rsvp { Yes, No, Maybe };

// Accepts exactly "Yes", "No", or "Maybe"
bool parseRsvp(const std::string& text, Rsvp& out);
const char* toString(Rsvp status);

bool isValidUtf8(const std::string& text);

// Splits on single spaces. Consecutive, leading, or trailing spaces produce
// empty fields, which the caller rejects as malformed.
std::vector<std::string> splitFields(const std::string& line);

// Non-empty, at most kMaxFieldBytes, no ASCII whitespace
bool isValidField(const std::string& field);

// MM/DD/YYYY and a real calendar date (leap years included)
bool isValidDate(const std::string& date);

// HH:MMAM or HH:MMPM with hour 01-12 and minute 00-59
bool isValidTime(const std::string& time);

namespace response {
inline const std::string kOk = "200 OK";
inline const std::string kCreatedEvent = "201 Created Event";
inline const std::string kDeletedEvent = "202 Deleted Event";
inline const std::string kFormatError = "300 Message Format Error";
inline const std::string kNotIdentified = "401 Not Identified";
inline const std::string kNotAuthorized = "402 Not Authorized";
inline const std::string kNotFound = "404 Not Found";
inline const std::string kConflict = "410 Conflict";
inline const std::string kEnd = "END";
}  // namespace response

namespace notify {
std::string itemClaimed(const std::string& eventCode, const std::string& item, const std::string& user);
std::string itemReleased(const std::string& eventCode, const std::string& item);
std::string eventUpdated(const std::string& eventCode, const std::string& date, const std::string& time);
std::string eventDeleted(const std::string& eventCode);
std::string rsvpUpdated(const std::string& eventCode, const std::string& user, Rsvp status);
}  // namespace notify

}  // namespace protocol
