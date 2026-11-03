#include "AppState.hpp"

using namespace protocol;

Guest* Event::findGuest(const std::string& userName) {
    for (Guest& guest : guests) {
        if (guest.userName == userName) {
            return &guest;
        }
    }
    return nullptr;
}

Item* Event::findItem(const std::string& itemName) {
    for (Item& item : items) {
        if (item.itemName == itemName) {
            return &item;
        }
    }
    return nullptr;
}

Result Result::single(const std::string& line, const std::string& note) {
    Result result;
    result.lines.push_back(line);
    result.note = note;
    return result;
}

Result Result::error(const std::string& status, const std::string& reason) {
    return single(status + ": " + reason, reason);
}

namespace {
const char* kNotJoined = "this connection has not joined that event";
}  // namespace

SessionId AppState::openSession() {
    const SessionId id = nextSessionId_++;
    sessions_[id].connectionId = id;
    return id;
}

std::size_t AppState::closeSession(SessionId session) {
    auto it = sessions_.find(session);
    if (it == sessions_.end()) {
        return 0;
    }
    const std::size_t identities = it->second.identities.size();
    sessions_.erase(it);
    return identities;
}

ClientSession& AppState::sessionFor(SessionId session) {
    ClientSession& entry = sessions_[session];
    entry.connectionId = session;
    return entry;
}

Event* AppState::identifiedEvent(SessionId session, const std::string& eventCode, std::string& userName) {
    auto sessionIt = sessions_.find(session);
    if (sessionIt == sessions_.end()) {
        return nullptr;
    }
    auto identityIt = sessionIt->second.identities.find(eventCode);
    if (identityIt == sessionIt->second.identities.end()) {
        return nullptr;
    }
    auto eventIt = events_.find(eventCode);
    if (eventIt == events_.end()) {
        return nullptr;
    }
    userName = identityIt->second;
    return &eventIt->second;
}

std::vector<SessionId> AppState::sessionsIn(const std::string& eventCode, const std::string* userName) const {
    std::vector<SessionId> result;
    for (const auto& [id, session] : sessions_) {
        auto identityIt = session.identities.find(eventCode);
        if (identityIt == session.identities.end()) {
            continue;
        }
        if (userName == nullptr || identityIt->second == *userName) {
            result.push_back(id);
        }
    }
    return result;
}

const Event* AppState::findEvent(const std::string& eventCode) const {
    auto it = events_.find(eventCode);
    return it == events_.end() ? nullptr : &it->second;
}

Result AppState::createEvent(SessionId session, const std::string& eventCode, const std::string& userName,
                             const std::string& date, const std::string& time) {
    if (events_.count(eventCode) != 0) {
        return Result::error(response::kConflict, "event code already in use");
    }

    Event event;
    event.eventCode = eventCode;
    event.organizerName = userName;
    event.date = date;
    event.time = time;
    event.guests.push_back(Guest{userName, Rsvp::Maybe, true});
    events_.emplace(eventCode, std::move(event));

    sessionFor(session).identities[eventCode] = userName;

    Result result;
    result.lines = {response::kCreatedEvent, "EventCode: " + eventCode};
    return result;
}

Result AppState::join(SessionId session, const std::string& eventCode, const std::string& userName) {
    auto eventIt = events_.find(eventCode);
    if (eventIt == events_.end()) {
        return Result::error(response::kNotFound, "no event with that code");
    }

    ClientSession& client = sessionFor(session);
    if (client.identities.count(eventCode) != 0) {
        return Result::error(response::kConflict, "you have already joined this event on this connection");
    }

    // An existing guest name is a rejoin: the guest keeps their RSVP status
    Event& event = eventIt->second;
    const bool rejoin = event.findGuest(userName) != nullptr;
    if (!rejoin) {
        event.guests.push_back(Guest{userName, Rsvp::Maybe, false});
    }
    client.identities[eventCode] = userName;

    Result result;
    result.lines = {response::kOk,
                    userName + ", you are now a guest of " + eventCode + " (" + event.date + " " + event.time + ")."};
    result.note = rejoin ? "rejoined as existing guest" : "new guest";
    return result;
}

Result AppState::deleteEvent(SessionId session, const std::string& eventCode) {
    std::string userName;
    Event* event = identifiedEvent(session, eventCode, userName);
    if (event == nullptr) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }
    if (userName != event->organizerName) {
        return Result::error(response::kNotAuthorized, "only the organizer can delete the event");
    }

    // Recipients are everyone identified in the event before it disappears
    const std::vector<SessionId> recipients = sessionsIn(eventCode);

    events_.erase(eventCode);
    for (auto& [id, client] : sessions_) {
        client.identities.erase(eventCode);
    }

    Result result = Result::single(response::kDeletedEvent);
    for (SessionId recipient : recipients) {
        result.notifications.push_back({recipient, notify::eventDeleted(eventCode)});
    }
    return result;
}

Result AppState::rsvp(SessionId session, const std::string& eventCode, Rsvp status) {
    std::string userName;
    Event* event = identifiedEvent(session, eventCode, userName);
    if (event == nullptr) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    Guest* guest = event->findGuest(userName);
    if (guest == nullptr) {
        // Identities always refer to a guest; guard anyway rather than crash
        return Result::error(response::kNotIdentified, kNotJoined);
    }
    guest->rsvpStatus = status;

    Result result = Result::single(response::kOk);
    for (SessionId recipient : sessionsIn(eventCode, &event->organizerName)) {
        result.notifications.push_back({recipient, notify::rsvpUpdated(eventCode, userName, status)});
    }
    return result;
}

Result AppState::itemList(SessionId session, const std::string& eventCode) {
    std::string userName;
    Event* event = identifiedEvent(session, eventCode, userName);
    if (event == nullptr) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    Result result = Result::single(response::kOk);
    for (const Item& item : event->items) {
        if (item.claimedBy.empty()) {
            result.lines.push_back(item.itemName + " - unclaimed");
        } else {
            result.lines.push_back(item.itemName + " - claimed by " + item.claimedBy);
        }
    }
    result.lines.push_back(response::kEnd);
    return result;
}

Result AppState::claimItem(SessionId session, const std::string& eventCode, const std::string& itemName) {
    std::string userName;
    Event* event = identifiedEvent(session, eventCode, userName);
    if (event == nullptr) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    Item* item = event->findItem(itemName);
    if (item != nullptr && !item->claimedBy.empty()) {
        return Result::error(response::kConflict, "item already claimed by " + item->claimedBy);
    }

    if (item == nullptr) {
        event->items.push_back(Item{itemName, userName});
    } else {
        item->claimedBy = userName;
    }

    Result result = Result::single(response::kOk);
    for (SessionId recipient : sessionsIn(eventCode)) {
        result.notifications.push_back({recipient, notify::itemClaimed(eventCode, itemName, userName)});
    }
    return result;
}

Result AppState::releaseItem(SessionId session, const std::string& eventCode, const std::string& itemName) {
    std::string userName;
    Event* event = identifiedEvent(session, eventCode, userName);
    if (event == nullptr) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    Item* item = event->findItem(itemName);
    if (item == nullptr) {
        return Result::error(response::kConflict, "that item is not on the list");
    }
    if (item->claimedBy.empty()) {
        return Result::error(response::kConflict, "that item is not claimed");
    }
    if (item->claimedBy != userName) {
        return Result::error(response::kConflict, "only " + item->claimedBy + " can release that item");
    }
    item->claimedBy.clear();

    Result result = Result::single(response::kOk);
    for (SessionId recipient : sessionsIn(eventCode)) {
        result.notifications.push_back({recipient, notify::itemReleased(eventCode, itemName)});
    }
    return result;
}

Result AppState::updateEvent(SessionId session, const std::string& eventCode, const std::string& date,
                             const std::string& time) {
    std::string userName;
    Event* event = identifiedEvent(session, eventCode, userName);
    if (event == nullptr) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }
    if (userName != event->organizerName) {
        return Result::error(response::kNotAuthorized, "only the organizer can update the event");
    }

    event->date = date;
    event->time = time;

    Result result = Result::single(response::kOk);
    for (SessionId recipient : sessionsIn(eventCode)) {
        result.notifications.push_back({recipient, notify::eventUpdated(eventCode, date, time)});
    }
    return result;
}

Result AppState::who(SessionId session, const std::string& eventCode) {
    std::string userName;
    Event* event = identifiedEvent(session, eventCode, userName);
    if (event == nullptr) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    Result result = Result::single(response::kOk);
    for (const Guest& guest : event->guests) {
        const std::string marker = guest.userName == event->organizerName ? " (organizer)" : "";
        result.lines.push_back(guest.userName + marker + " - " + toString(guest.rsvpStatus));
    }
    result.lines.push_back(response::kEnd);
    return result;
}
