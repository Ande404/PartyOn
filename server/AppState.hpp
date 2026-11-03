#pragma once

// PartyOn application state (Phase 1 data design, section 8.3).
// Handlers assume their arguments are already format-validated by the
// Dispatcher; they only check identity, authorization, and state conflicts
// (401 / 402 / 404 / 410). They never touch sockets: each returns a Result
// that the network layer writes as one response block followed by any
// notifications.

#include <map>
#include <string>
#include <vector>

#include "Protocol.hpp"

using SessionId = int;

struct Guest {
    std::string userName;
    protocol::Rsvp rsvpStatus = protocol::Rsvp::Maybe;
    bool isOrganizer = false;
};

struct Item {
    std::string itemName;
    std::string claimedBy;  // empty when unclaimed
};

struct Event {
    std::string eventCode;
    std::string organizerName;
    std::string date;
    std::string time;
    std::vector<Guest> guests;  // kept in join order
    std::vector<Item> items;    // kept in first-claim order

    Guest* findGuest(const std::string& userName);
    Item* findItem(const std::string& itemName);
};

struct ClientSession {
    SessionId connectionId = 0;
    std::map<std::string, std::string> identities;  // eventCode -> userName
};

struct Notification {
    SessionId recipient;
    std::string line;
};

struct Result {
    std::vector<std::string> lines;            // response block, sent first
    std::vector<Notification> notifications;   // sent after the response block
    bool closeConnection = false;              // QUIT
    std::string note;                          // reason, for the server log only

    static Result single(const std::string& line, const std::string& note = "");

    // One-line error sent as "<code> <phrase>: <reason>" so the user can see
    // what to fix; the reason is also kept as the log note
    static Result error(const std::string& status, const std::string& reason);
};

class AppState {
public:
    SessionId openSession();

    // Removes the session and all of its identities. Guest records stay in
    // their events so the user can JOIN again. Returns the identity count.
    std::size_t closeSession(SessionId session);

    Result createEvent(SessionId session, const std::string& eventCode, const std::string& userName,
                       const std::string& date, const std::string& time);
    Result join(SessionId session, const std::string& eventCode, const std::string& userName);
    Result deleteEvent(SessionId session, const std::string& eventCode);
    Result rsvp(SessionId session, const std::string& eventCode, protocol::Rsvp status);
    Result itemList(SessionId session, const std::string& eventCode);
    Result claimItem(SessionId session, const std::string& eventCode, const std::string& itemName);
    Result releaseItem(SessionId session, const std::string& eventCode, const std::string& itemName);
    Result updateEvent(SessionId session, const std::string& eventCode, const std::string& date,
                       const std::string& time);
    Result who(SessionId session, const std::string& eventCode);

    const Event* findEvent(const std::string& eventCode) const;

private:
    ClientSession& sessionFor(SessionId session);

    // The event this session is identified in, with the username it uses
    // there; nullptr when not identified (including events that do not exist)
    Event* identifiedEvent(SessionId session, const std::string& eventCode, std::string& userName);

    // Sessions identified in the event, optionally only those using userName
    std::vector<SessionId> sessionsIn(const std::string& eventCode, const std::string* userName = nullptr) const;

    std::map<std::string, Event> events_;
    std::map<SessionId, ClientSession> sessions_;
    SessionId nextSessionId_ = 1;
};
