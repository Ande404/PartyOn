#pragma once

// PartyOn application state (Phase 1 data design, section 8.3), safe to use
// from many client threads at once (protocol spec v2, Concurrency and
// Conflict Behavior).
//
// Handlers assume their arguments are already format-validated by the
// Dispatcher; they only check identity, authorization, and state conflicts
// (401 / 402 / 404 / 410). They never touch sockets.
//
// Locking:
//   - eventsMutex_ guards the events_ map only (lookup, insert, erase).
//   - Each event has its own mutex. A command for an event runs entirely
//     under that event's lock, so commands for one event are applied one at a
//     time (first come, first served) while different events run in parallel.
//   - Lock order: an event lock may be followed by eventsMutex_ (DELETE) or
//     sessionsMutex_ (JOIN / CREATE). Code never holds two event locks, and
//     never takes an event lock while holding eventsMutex_ or sessionsMutex_,
//     except CREATE locking a new event that no other thread can see yet.
//   - When a sink is set, a command's response and notifications are handed
//     to it while the event lock is still held, so every connection receives
//     an event's notifications in the order the changes were applied.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

struct Notification {
    SessionId recipient;
    std::string line;
};

struct Result {
    std::vector<std::string> lines;            // response block, sent first
    std::vector<Notification> notifications;   // sent after the response block
    bool closeConnection = false;              // QUIT
    bool delivered = false;                    // already handed to the sink
    std::string note;                          // reason, for the server log only
    std::string command;                       // for the server log
    std::string eventCode;                     // for the server log

    static Result single(const std::string& line, const std::string& note = "");

    // One-line error sent as "<code> <phrase>: <reason>" so the user can see
    // what to fix; the reason is also kept as the log note
    static Result error(const std::string& status, const std::string& reason);
};

class AppState {
public:
    // Receives one block of complete lines (each ending in '\n') for a session
    using Sink = std::function<void(SessionId, const std::string& block)>;

    // Set once before clients are served. Without a sink (unit tests) results
    // are only returned, never delivered.
    void setSink(Sink sink);

    SessionId openSession();

    // Removes the session from every event it is identified in. Guest
    // records stay so the user can JOIN again. Returns the identity count.
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

    // A copy of the event as it is right now; empty if it does not exist
    std::optional<Event> snapshot(const std::string& eventCode) const;

private:
    struct EventEntry {
        std::mutex mutex;
        Event event;
        bool deleted = false;                          // set by DELETE_EVENT
        std::map<SessionId, std::string> connections;  // identified sessions -> username
    };

    std::shared_ptr<EventEntry> lookup(const std::string& eventCode) const;

    // Username this session uses in the event, or nullptr. Caller holds the event lock.
    static const std::string* identity(const EventEntry& entry, SessionId session);

    // Connections identified in the event, optionally only those using userName.
    // Caller holds the event lock.
    static std::vector<SessionId> recipients(const EventEntry& entry, const std::string* userName = nullptr);

    // Hands the response and notifications to the sink. Caller holds the event lock.
    Result publish(SessionId requester, Result result) const;

    void track(SessionId session, const std::string& eventCode);

    mutable std::mutex eventsMutex_;
    std::map<std::string, std::shared_ptr<EventEntry>> events_;

    std::mutex sessionsMutex_;
    std::map<SessionId, std::set<std::string>> sessionEvents_;  // events each session joined or created

    std::atomic<SessionId> nextSessionId_{1};
    Sink sink_;
};
