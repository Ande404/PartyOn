#include "Protocol.hpp"

namespace protocol {

namespace {

bool isAsciiWhitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

int twoDigits(const std::string& text, std::size_t pos) {
    return (text[pos] - '0') * 10 + (text[pos + 1] - '0');
}

bool isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

}  // namespace

bool parseRsvp(const std::string& text, Rsvp& out) {
    if (text == "Yes") {
        out = Rsvp::Yes;
    } else if (text == "No") {
        out = Rsvp::No;
    } else if (text == "Maybe") {
        out = Rsvp::Maybe;
    } else {
        return false;
    }
    return true;
}

const char* toString(Rsvp status) {
    switch (status) {
        case Rsvp::Yes: return "Yes";
        case Rsvp::No: return "No";
        case Rsvp::Maybe: return "Maybe";
    }
    return "Maybe";
}

// Rejects overlong encodings, UTF-16 surrogates, and code points above U+10FFFF
bool isValidUtf8(const std::string& text) {
    const std::size_t size = text.size();
    std::size_t i = 0;

    while (i < size) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }

        std::size_t length;
        unsigned char secondMin = 0x80;
        unsigned char secondMax = 0xBF;

        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            secondMin = 0xA0;
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            secondMax = 0x9F;
        } else if (lead == 0xF0) {
            length = 4;
            secondMin = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            secondMax = 0x8F;
        } else {
            return false;
        }

        if (i + length > size) {
            return false;
        }

        const unsigned char second = static_cast<unsigned char>(text[i + 1]);
        if (second < secondMin || second > secondMax) {
            return false;
        }

        for (std::size_t k = 2; k < length; ++k) {
            const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
            if (continuation < 0x80 || continuation > 0xBF) {
                return false;
            }
        }

        i += length;
    }

    return true;
}

std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;

    while (true) {
        const std::size_t space = line.find(' ', start);
        if (space == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, space - start));
        start = space + 1;
    }

    return fields;
}

bool isValidField(const std::string& field) {
    if (field.empty() || field.size() > kMaxFieldBytes) {
        return false;
    }
    for (unsigned char c : field) {
        if (isAsciiWhitespace(c)) {
            return false;
        }
    }
    return true;
}

bool isValidDate(const std::string& date) {
    if (date.size() != 10 || date[2] != '/' || date[5] != '/') {
        return false;
    }
    for (std::size_t pos : {0, 1, 3, 4, 6, 7, 8, 9}) {
        if (!isDigit(date[pos])) {
            return false;
        }
    }

    const int month = twoDigits(date, 0);
    const int day = twoDigits(date, 3);
    const int year = twoDigits(date, 6) * 100 + twoDigits(date, 8);

    if (year < 1 || month < 1 || month > 12 || day < 1) {
        return false;
    }

    static const int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxDay = kDaysInMonth[month - 1];
    if (month == 2 && isLeapYear(year)) {
        maxDay = 29;
    }

    return day <= maxDay;
}

bool isValidTime(const std::string& time) {
    if (time.size() != 7 || time[2] != ':') {
        return false;
    }
    if (!isDigit(time[0]) || !isDigit(time[1]) || !isDigit(time[3]) || !isDigit(time[4])) {
        return false;
    }

    const std::string suffix = time.substr(5);
    if (suffix != "AM" && suffix != "PM") {
        return false;
    }

    const int hour = twoDigits(time, 0);
    const int minute = twoDigits(time, 3);

    return hour >= 1 && hour <= 12 && minute <= 59;
}

namespace notify {

std::string itemClaimed(const std::string& eventCode, const std::string& item, const std::string& user) {
    return "210 " + eventCode + " ITEM_CLAIMED " + item + " claimed by " + user;
}

std::string itemReleased(const std::string& eventCode, const std::string& item) {
    return "210 " + eventCode + " ITEM_RELEASED " + item + " is available to bring again";
}

std::string eventUpdated(const std::string& eventCode, const std::string& date, const std::string& time) {
    return "210 " + eventCode + " EVENT_UPDATED new event details: " + date + " " + time;
}

std::string eventDeleted(const std::string& eventCode) {
    return "210 " + eventCode + " EVENT_DELETED This event has been deleted";
}

std::string rsvpUpdated(const std::string& eventCode, const std::string& user, Rsvp status) {
    return "210 " + eventCode + " RSVP_UPDATED " + user + " status is now " + toString(status);
}

}  // namespace notify

}  // namespace protocol
