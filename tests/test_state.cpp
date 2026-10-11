// Unit tests for Dispatcher + AppState: every command, every response code,
// error precedence, and notification recipients. No network involved.

#include <string>
#include <vector>

#include "AppState.hpp"
#include "Dispatcher.hpp"
#include "TestUtil.hpp"

namespace {

using Lines = std::vector<std::string>;

struct Harness {
    AppState state;
    Dispatcher dispatcher{state};

    Result send(SessionId session, const std::string& line) {
        return dispatcher.handle(session, line);
    }

    Lines lines(SessionId session, const std::string& line) {
        return send(session, line).lines;
    }
};

std::vector<SessionId> recipients(const Result& result) {
    std::vector<SessionId> ids;
    for (const Notification& n : result.notifications) {
        ids.push_back(n.recipient);
    }
    return ids;
}

const Lines kFormatError = {"300 Message Format Error"};
const Lines kNotIdentified = {"401 Not Identified"};
const Lines kNotAuthorized = {"402 Not Authorized"};
const Lines kNotFound = {"404 Not Found"};
const Lines kConflict = {"410 Conflict"};
const Lines kOk = {"200 OK"};

void formatErrors() {
    Harness h;
    SessionId s = h.state.openSession();

    CHECK_EQ(h.lines(s, ""), kFormatError);                              // empty line
    CHECK_EQ(h.lines(s, "join Party1 User1"), kFormatError);             // lowercase
    CHECK_EQ(h.lines(s, "HELP"), kFormatError);                          // client-side only
    CHECK_EQ(h.lines(s, "DANCE Party1"), kFormatError);                  // unknown
    CHECK_EQ(h.lines(s, "WHO"), kFormatError);                           // missing field
    CHECK_EQ(h.lines(s, "WHO Party1 extra"), kFormatError);              // extra field
    CHECK_EQ(h.lines(s, "WHO  Party1"), kFormatError);                   // double space
    CHECK_EQ(h.lines(s, " WHO Party1"), kFormatError);                   // leading space
    CHECK_EQ(h.lines(s, "WHO Party1 "), kFormatError);                   // trailing space
    CHECK_EQ(h.lines(s, "WHO Par\tty1"), kFormatError);                  // tab in field
    CHECK_EQ(h.lines(s, "JOIN Party1 Bad\xFF"), kFormatError);           // invalid UTF-8
    CHECK_EQ(h.lines(s, "JOIN Party1 " + std::string(101, 'u')), kFormatError);
    CHECK_EQ(h.lines(s, "QUIT now"), kFormatError);
    CHECK_EQ(Dispatcher::oversizedLine().lines, kFormatError);

    // Bad dates and times
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 02/30/2026 03:00PM"), kFormatError);
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 02/29/2026 03:00PM"), kFormatError);  // not a leap year
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 13/01/2026 03:00PM"), kFormatError);
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 9/22/2026 03:00PM"), kFormatError);
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 09/22/2026 13:00PM"), kFormatError);
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 09/22/2026 00:30AM"), kFormatError);
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 09/22/2026 03:60PM"), kFormatError);
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 09/22/2026 03:00pm"), kFormatError);
    CHECK_EQ(h.lines(s, "CREATE_EVENT P U 09/22/2026 03:00 PM"), kFormatError);

    // 100-byte fields and multi-byte UTF-8 names are fine
    CHECK_EQ(h.lines(s, "CREATE_EVENT " + std::string(100, 'e') + " Jos\xC3\xA9 02/29/2028 12:00AM")[0],
             std::string("201 Created Event"));
}

void createAndJoin() {
    Harness h;
    SessionId organizer = h.state.openSession();
    SessionId guest = h.state.openSession();

    CHECK_EQ(h.lines(organizer, "CREATE_EVENT Party1 Andrew 09/22/2026 03:00PM"),
             (Lines{"201 Created Event", "EventCode: Party1"}));
    CHECK_EQ(h.lines(guest, "CREATE_EVENT Party1 Other 09/22/2026 03:00PM"), kConflict);
    CHECK_EQ(h.lines(organizer, "CREATE_EVENT Party1 Andrew 09/22/2026 03:00PM"), kConflict);

    CHECK_EQ(h.lines(guest, "JOIN Nope Sam"), kNotFound);
    CHECK_EQ(h.lines(guest, "JOIN Party1 Sam"),
             (Lines{"200 OK", "Sam, you are now a guest of Party1 (09/22/2026 03:00PM)."}));
    CHECK_EQ(h.lines(guest, "JOIN Party1 Sam"), kConflict);    // already identified
    CHECK_EQ(h.lines(guest, "JOIN Party1 Other"), kConflict);  // even with another name
    CHECK_EQ(h.lines(organizer, "JOIN Party1 Andrew"), kConflict);

    // Precedence for JOIN: 300 before 404
    CHECK_EQ(h.lines(guest, "JOIN Nope"), kFormatError);

    CHECK_EQ(h.lines(organizer, "WHO Party1"),
             (Lines{"200 OK", "Andrew (organizer) - Maybe", "Sam - Maybe", "END"}));
}

void rejoinKeepsRsvp() {
    Harness h;
    SessionId organizer = h.state.openSession();
    SessionId first = h.state.openSession();
    h.send(organizer, "CREATE_EVENT Party1 Andrew 09/22/2026 03:00PM");
    h.send(first, "JOIN Party1 Sam");
    h.send(first, "RSVP Party1 Yes");

    CHECK_EQ(h.state.closeSession(first), 1u);

    SessionId second = h.state.openSession();
    CHECK_EQ(h.lines(second, "WHO Party1"), kNotIdentified);  // identities do not survive reconnecting
    Result rejoin = h.send(second, "JOIN Party1 Sam");
    CHECK_EQ(rejoin.lines[0], std::string("200 OK"));
    CHECK_EQ(rejoin.note, std::string("rejoined as existing guest"));
    CHECK_EQ(h.lines(second, "WHO Party1"), (Lines{"200 OK", "Andrew (organizer) - Maybe", "Sam - Yes", "END"}));
}

void rsvp() {
    Harness h;
    SessionId organizer = h.state.openSession();
    SessionId guest = h.state.openSession();

    CHECK_EQ(h.lines(guest, "RSVP Party1 Yes"), kNotIdentified);
    CHECK_EQ(h.lines(guest, "RSVP Party1 Purple"), kFormatError);  // 300 before 401

    h.send(organizer, "CREATE_EVENT Party1 Andrew 09/22/2026 03:00PM");
    h.send(guest, "JOIN Party1 Sam");

    CHECK_EQ(h.lines(guest, "RSVP Party1 yes"), kFormatError);

    // Guest RSVP notifies the organizer only
    Result result = h.send(guest, "RSVP Party1 No");
    CHECK_EQ(result.lines, kOk);
    CHECK_EQ(result.notifications.size(), 1u);
    CHECK_EQ(result.notifications[0].recipient, organizer);
    CHECK_EQ(result.notifications[0].line, std::string("210 Party1 RSVP_UPDATED Sam status is now No"));

    // Organizer RSVP notifies the organizer's own connection
    result = h.send(organizer, "RSVP Party1 Yes");
    CHECK_EQ(recipients(result), std::vector<SessionId>{organizer});
    CHECK_EQ(result.notifications[0].line, std::string("210 Party1 RSVP_UPDATED Andrew status is now Yes"));

    CHECK_EQ(h.lines(guest, "WHO Party1"), (Lines{"200 OK", "Andrew (organizer) - Yes", "Sam - No", "END"}));
}

void items() {
    Harness h;
    SessionId organizer = h.state.openSession();
    SessionId guest = h.state.openSession();
    SessionId outsider = h.state.openSession();
    h.send(organizer, "CREATE_EVENT Party1 Andrew 09/22/2026 03:00PM");
    h.send(guest, "JOIN Party1 Sam");

    CHECK_EQ(h.lines(guest, "ITEMLIST Party1"), (Lines{"200 OK", "END"}));
    CHECK_EQ(h.lines(outsider, "ITEMLIST Party1"), kNotIdentified);
    CHECK_EQ(h.lines(outsider, "CLAIM_ITEM Party1 Chips"), kNotIdentified);

    // Claim notifies every connection identified in the event
    Result claim = h.send(guest, "CLAIM_ITEM Party1 Chips");
    CHECK_EQ(claim.lines, kOk);
    CHECK_EQ(recipients(claim), (std::vector<SessionId>{organizer, guest}));
    CHECK_EQ(claim.notifications[0].line, std::string("210 Party1 ITEM_CLAIMED Chips claimed by Sam"));

    CHECK_EQ(h.lines(organizer, "CLAIM_ITEM Party1 Chips"), kConflict);  // someone else's
    CHECK_EQ(h.lines(guest, "CLAIM_ITEM Party1 Chips"), kConflict);      // own claim too
    CHECK_EQ(h.lines(organizer, "RELEASE_ITEM Party1 Chips"), kConflict);  // not theirs
    CHECK_EQ(h.lines(guest, "RELEASE_ITEM Party1 Soda"), kConflict);       // not on list

    Result release = h.send(guest, "RELEASE_ITEM Party1 Chips");
    CHECK_EQ(release.lines, kOk);
    CHECK_EQ(recipients(release), (std::vector<SessionId>{organizer, guest}));
    CHECK_EQ(release.notifications[0].line, std::string("210 Party1 ITEM_RELEASED Chips is available to bring again"));
    CHECK_EQ(h.lines(guest, "RELEASE_ITEM Party1 Chips"), kConflict);  // already unclaimed

    h.send(organizer, "CLAIM_ITEM Party1 Soda");
    CHECK_EQ(h.lines(guest, "ITEMLIST Party1"),
             (Lines{"200 OK", "Chips - unclaimed", "Soda - claimed by Andrew", "END"}));

    // A released item can be claimed again by anyone
    CHECK_EQ(h.lines(organizer, "CLAIM_ITEM Party1 Chips"), kOk);
    CHECK_EQ(h.lines(guest, "ITEMLIST Party1"),
             (Lines{"200 OK", "Chips - claimed by Andrew", "Soda - claimed by Andrew", "END"}));

    // Item names are case-sensitive
    CHECK_EQ(h.lines(guest, "CLAIM_ITEM Party1 chips"), kOk);
}

void updateEvent() {
    Harness h;
    SessionId organizer = h.state.openSession();
    SessionId guest = h.state.openSession();
    h.send(organizer, "CREATE_EVENT Party1 Andrew 09/22/2026 03:00PM");
    h.send(guest, "JOIN Party1 Sam");

    CHECK_EQ(h.lines(guest, "UPDATE_EVENT Party1 10/15/2026 06:00PM"), kNotAuthorized);
    CHECK_EQ(h.lines(guest, "UPDATE_EVENT Party1 10/15/2026 6:00PM"), kFormatError);  // 300 before 402
    CHECK_EQ(h.lines(guest, "UPDATE_EVENT Other 10/15/2026 06:00PM"), kNotIdentified);

    Result result = h.send(organizer, "UPDATE_EVENT Party1 10/15/2026 06:00PM");
    CHECK_EQ(result.lines, kOk);
    CHECK_EQ(recipients(result), (std::vector<SessionId>{organizer, guest}));
    CHECK_EQ(result.notifications[0].line,
             std::string("210 Party1 EVENT_UPDATED new event details: 10/15/2026 06:00PM"));

    SessionId late = h.state.openSession();
    CHECK_EQ(h.lines(late, "JOIN Party1 Kim")[1], std::string("Kim, you are now a guest of Party1 (10/15/2026 06:00PM)."));
}

void deleteEvent() {
    Harness h;
    SessionId organizer = h.state.openSession();
    SessionId guest = h.state.openSession();
    h.send(organizer, "CREATE_EVENT Party1 Andrew 09/22/2026 03:00PM");
    h.send(guest, "JOIN Party1 Sam");

    CHECK_EQ(h.lines(guest, "DELETE_EVENT Party1"), kNotAuthorized);
    CHECK_EQ(h.lines(guest, "DELETE_EVENT Nope"), kNotIdentified);

    Result result = h.send(organizer, "DELETE_EVENT Party1");
    CHECK_EQ(result.lines, (Lines{"202 Deleted Event"}));
    CHECK_EQ(recipients(result), (std::vector<SessionId>{organizer, guest}));
    CHECK_EQ(result.notifications[0].line, std::string("210 Party1 EVENT_DELETED This event has been deleted"));
    CHECK(h.state.findEvent("Party1") == nullptr);

    // Deleted event: 401 for everything except JOIN, which gets 404
    CHECK_EQ(h.lines(guest, "WHO Party1"), kNotIdentified);
    CHECK_EQ(h.lines(organizer, "DELETE_EVENT Party1"), kNotIdentified);
    CHECK_EQ(h.lines(guest, "JOIN Party1 Sam"), kNotFound);

    // The code can be reused, and old identities do not carry over
    SessionId other = h.state.openSession();
    CHECK_EQ(h.lines(other, "CREATE_EVENT Party1 Kim 12/01/2026 01:00PM")[0], std::string("201 Created Event"));
    CHECK_EQ(h.lines(guest, "WHO Party1"), kNotIdentified);
    CHECK_EQ(h.lines(organizer, "UPDATE_EVENT Party1 12/02/2026 01:00PM"), kNotIdentified);
}

void multipleEventsPerConnection() {
    Harness h;
    SessionId s = h.state.openSession();
    h.send(s, "CREATE_EVENT A Andrew 09/22/2026 03:00PM");
    h.send(s, "CREATE_EVENT B Drew 09/23/2026 03:00PM");

    CHECK_EQ(h.lines(s, "WHO A"), (Lines{"200 OK", "Andrew (organizer) - Maybe", "END"}));
    CHECK_EQ(h.lines(s, "WHO B"), (Lines{"200 OK", "Drew (organizer) - Maybe", "END"}));
    CHECK_EQ(h.send(s, "CLAIM_ITEM B Cake").notifications[0].line, std::string("210 B ITEM_CLAIMED Cake claimed by Drew"));
    CHECK_EQ(h.state.closeSession(s), 2u);
}

void quit() {
    Harness h;
    SessionId s = h.state.openSession();
    Result result = h.send(s, "QUIT");
    CHECK(result.closeConnection);
    CHECK(result.lines.empty());
    CHECK(result.notifications.empty());
}

}  // namespace

int main() {
    formatErrors();
    createAndJoin();
    rejoinKeepsRsvp();
    rsvp();
    items();
    updateEvent();
    deleteEvent();
    multipleEventsPerConnection();
    quit();
    return testutil::report("test_state");
}
