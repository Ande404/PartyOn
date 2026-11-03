#include "AppState.hpp"

#include "EventStore.hpp"
#include "Logger.hpp"

using namespace protocol;

namespace {

const char* kNotJoined = "this connection has not joined that event";
const char* kNoSuchEvent = "no event with that code";

std::string joinLines(const std::vector<std::string>& lines) {
    std::string block;
    for (const std::string& line : lines) {
        block += line + "\n";
    }
    return block;
}

}  // namespace

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

void AppState::setSink(Sink sink) {
    sink_ = std::move(sink);
}

void AppState::setStore(const EventStore* store) {
    store_ = store;
}

bool AppState::restore(const Event& event) {
    auto entry = std::make_shared<EventEntry>();
    entry->event = event;
    std::lock_guard<std::mutex> lock(eventsMutex_);
    return events_.emplace(event.eventCode, entry).second;
}

void AppState::persist(const Event& event) const {
    std::string error;
    if (store_ != nullptr && !store_->save(event, error)) {
        // The change stays in memory; see Known limitations in the README
        logger::error("could not save event " + event.eventCode + ": " + error);
    }
}

void AppState::unpersist(const std::string& eventCode) const {
    std::string error;
    if (store_ != nullptr && !store_->remove(eventCode, error)) {
        logger::error(error);
    }
}

SessionId AppState::openSession() {
    const SessionId id = nextSessionId_++;
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessionEvents_[id];
    return id;
}

std::size_t AppState::closeSession(SessionId session) {
    std::set<std::string> eventCodes;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessionEvents_.find(session);
        if (it == sessionEvents_.end()) {
            return 0;
        }
        eventCodes = std::move(it->second);
        sessionEvents_.erase(it);
    }

    // One event lock at a time, never while holding sessionsMutex_
    std::size_t identities = 0;
    for (const std::string& eventCode : eventCodes) {
        if (auto entry = lookup(eventCode)) {
            std::lock_guard<std::mutex> lock(entry->mutex);
            identities += entry->connections.erase(session);
        }
    }
    return identities;
}

std::shared_ptr<AppState::EventEntry> AppState::lookup(const std::string& eventCode) const {
    std::lock_guard<std::mutex> lock(eventsMutex_);
    auto it = events_.find(eventCode);
    return it == events_.end() ? nullptr : it->second;
}

const std::string* AppState::identity(const EventEntry& entry, SessionId session) {
    if (entry.deleted) {
        return nullptr;
    }
    auto it = entry.connections.find(session);
    return it == entry.connections.end() ? nullptr : &it->second;
}

std::vector<SessionId> AppState::recipients(const EventEntry& entry, const std::string* userName) {
    std::vector<SessionId> result;
    for (const auto& [id, name] : entry.connections) {
        if (userName == nullptr || name == *userName) {
            result.push_back(id);
        }
    }
    return result;
}

Result AppState::publish(SessionId requester, Result result) const {
    if (sink_) {
        if (!result.lines.empty()) {
            sink_(requester, joinLines(result.lines));
        }
        for (const Notification& notification : result.notifications) {
            sink_(notification.recipient, notification.line + "\n");
        }
        result.delivered = true;
    }
    return result;
}

void AppState::track(SessionId session, const std::string& eventCode) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessionEvents_[session].insert(eventCode);
}

std::optional<Event> AppState::snapshot(const std::string& eventCode) const {
    auto entry = lookup(eventCode);
    if (!entry) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->deleted) {
        return std::nullopt;
    }
    return entry->event;
}

Result AppState::createEvent(SessionId session, const std::string& eventCode, const std::string& userName,
                             const std::string& date, const std::string& time) {
    auto entry = std::make_shared<EventEntry>();
    std::unique_lock<std::mutex> eventLock(entry->mutex);  // nobody else can see it yet

    entry->event.eventCode = eventCode;
    entry->event.organizerName = userName;
    entry->event.date = date;
    entry->event.time = time;
    entry->event.guests.push_back(Guest{userName, Rsvp::Maybe, true});
    entry->connections[session] = userName;

    {
        // Creates are ordered by eventsMutex_: the first one for a code wins
        std::lock_guard<std::mutex> mapLock(eventsMutex_);
        if (events_.count(eventCode) != 0) {
            return Result::error(response::kConflict, "event code already in use");
        }
        events_.emplace(eventCode, entry);
    }
    persist(entry->event);
    track(session, eventCode);

    Result result;
    result.lines = {response::kCreatedEvent, "EventCode: " + eventCode};
    return publish(session, std::move(result));
}

Result AppState::join(SessionId session, const std::string& eventCode, const std::string& userName) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotFound, kNoSuchEvent);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->deleted) {
        // Deleted while this command waited for the lock
        return publish(session, Result::error(response::kNotFound, kNoSuchEvent));
    }
    if (entry->connections.count(session) != 0) {
        return publish(session,
                       Result::error(response::kConflict, "you have already joined this event on this connection"));
    }

    // An existing guest name is a rejoin: the guest keeps their RSVP status
    Event& event = entry->event;
    const bool rejoin = event.findGuest(userName) != nullptr;
    if (!rejoin) {
        event.guests.push_back(Guest{userName, Rsvp::Maybe, false});
        persist(event);
    }
    entry->connections[session] = userName;
    track(session, eventCode);

    Result result;
    result.lines = {response::kOk,
                    userName + ", you are now a guest of " + eventCode + " (" + event.date + " " + event.time + ")."};
    result.note = rejoin ? "rejoined as existing guest" : "new guest";
    return publish(session, std::move(result));
}

Result AppState::deleteEvent(SessionId session, const std::string& eventCode) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    const std::string* userName = identity(*entry, session);
    if (userName == nullptr) {
        return publish(session, Result::error(response::kNotIdentified, kNotJoined));
    }
    if (*userName != entry->event.organizerName) {
        return publish(session, Result::error(response::kNotAuthorized, "only the organizer can delete the event"));
    }

    // Recipients are everyone identified in the event before it disappears
    const std::vector<SessionId> notified = recipients(*entry);

    entry->deleted = true;
    entry->connections.clear();  // no connection keeps access, even if the code is reused

    // Delete the file before freeing the code, so a CREATE that reuses the
    // code cannot save its new file and then have it deleted here
    unpersist(eventCode);
    {
        std::lock_guard<std::mutex> mapLock(eventsMutex_);
        auto it = events_.find(eventCode);
        if (it != events_.end() && it->second == entry) {
            events_.erase(it);
        }
    }

    Result result = Result::single(response::kDeletedEvent);
    for (SessionId recipient : notified) {
        result.notifications.push_back({recipient, notify::eventDeleted(eventCode)});
    }
    return publish(session, std::move(result));
}

Result AppState::rsvp(SessionId session, const std::string& eventCode, Rsvp status) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    const std::string* userName = identity(*entry, session);
    Guest* guest = userName == nullptr ? nullptr : entry->event.findGuest(*userName);
    if (guest == nullptr) {
        return publish(session, Result::error(response::kNotIdentified, kNotJoined));
    }
    guest->rsvpStatus = status;

    // PartyOn's last-minute-change feature: a guest who can no longer come
    // stops being responsible for their items, so they become available
    // again in the same step and everyone sees what still needs covering
    std::vector<std::string> released;
    if (status == Rsvp::No) {
        for (Item& item : entry->event.items) {
            if (item.claimedBy == *userName) {
                item.claimedBy.clear();
                released.push_back(item.itemName);
            }
        }
    }
    persist(entry->event);

    Result result = Result::single(response::kOk);
    for (SessionId recipient : recipients(*entry, &entry->event.organizerName)) {
        result.notifications.push_back({recipient, notify::rsvpUpdated(eventCode, *userName, status)});
    }
    const std::vector<SessionId> everyone = recipients(*entry);
    for (const std::string& itemName : released) {
        for (SessionId recipient : everyone) {
            result.notifications.push_back({recipient, notify::itemReleased(eventCode, itemName)});
        }
    }
    if (!released.empty()) {
        result.note = "released " + std::to_string(released.size()) + (released.size() == 1 ? " item" : " items");
    }
    return publish(session, std::move(result));
}

Result AppState::itemList(SessionId session, const std::string& eventCode) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    if (identity(*entry, session) == nullptr) {
        return publish(session, Result::error(response::kNotIdentified, kNotJoined));
    }

    Result result = Result::single(response::kOk);
    for (const Item& item : entry->event.items) {
        if (item.claimedBy.empty()) {
            result.lines.push_back(item.itemName + " - unclaimed");
        } else {
            result.lines.push_back(item.itemName + " - claimed by " + item.claimedBy);
        }
    }
    result.lines.push_back(response::kEnd);
    return publish(session, std::move(result));
}

Result AppState::claimItem(SessionId session, const std::string& eventCode, const std::string& itemName) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    const std::string* userName = identity(*entry, session);
    if (userName == nullptr) {
        return publish(session, Result::error(response::kNotIdentified, kNotJoined));
    }

    // "No" means not attending and not bringing anything (RSVP No released
    // the guest's items). Someone who still brings things RSVPs Maybe or Yes.
    const Guest* guest = entry->event.findGuest(*userName);
    if (guest != nullptr && guest->rsvpStatus == Rsvp::No) {
        return publish(session, Result::error(response::kConflict,
                                              "you RSVP'd No; change your RSVP to Yes or Maybe to claim items"));
    }

    Item* item = entry->event.findItem(itemName);
    if (item != nullptr && !item->claimedBy.empty()) {
        return publish(session, Result::error(response::kConflict, "item already claimed by " + item->claimedBy));
    }

    if (item == nullptr) {
        entry->event.items.push_back(Item{itemName, *userName});
    } else {
        item->claimedBy = *userName;
    }
    persist(entry->event);

    Result result = Result::single(response::kOk);
    for (SessionId recipient : recipients(*entry)) {
        result.notifications.push_back({recipient, notify::itemClaimed(eventCode, itemName, *userName)});
    }
    return publish(session, std::move(result));
}

Result AppState::releaseItem(SessionId session, const std::string& eventCode, const std::string& itemName) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    const std::string* userName = identity(*entry, session);
    if (userName == nullptr) {
        return publish(session, Result::error(response::kNotIdentified, kNotJoined));
    }

    Item* item = entry->event.findItem(itemName);
    if (item == nullptr) {
        return publish(session, Result::error(response::kConflict, "that item is not on the list"));
    }
    if (item->claimedBy.empty()) {
        return publish(session, Result::error(response::kConflict, "that item is not claimed"));
    }
    if (item->claimedBy != *userName) {
        return publish(session,
                       Result::error(response::kConflict, "only " + item->claimedBy + " can release that item"));
    }
    item->claimedBy.clear();
    persist(entry->event);

    Result result = Result::single(response::kOk);
    for (SessionId recipient : recipients(*entry)) {
        result.notifications.push_back({recipient, notify::itemReleased(eventCode, itemName)});
    }
    return publish(session, std::move(result));
}

Result AppState::updateEvent(SessionId session, const std::string& eventCode, const std::string& date,
                             const std::string& time) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    const std::string* userName = identity(*entry, session);
    if (userName == nullptr) {
        return publish(session, Result::error(response::kNotIdentified, kNotJoined));
    }
    if (*userName != entry->event.organizerName) {
        return publish(session, Result::error(response::kNotAuthorized, "only the organizer can update the event"));
    }

    entry->event.date = date;
    entry->event.time = time;
    persist(entry->event);

    Result result = Result::single(response::kOk);
    for (SessionId recipient : recipients(*entry)) {
        result.notifications.push_back({recipient, notify::eventUpdated(eventCode, date, time)});
    }
    return publish(session, std::move(result));
}

Result AppState::who(SessionId session, const std::string& eventCode) {
    auto entry = lookup(eventCode);
    if (!entry) {
        return Result::error(response::kNotIdentified, kNotJoined);
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    if (identity(*entry, session) == nullptr) {
        return publish(session, Result::error(response::kNotIdentified, kNotJoined));
    }

    const Event& event = entry->event;
    Result result = Result::single(response::kOk);
    for (const Guest& guest : event.guests) {
        const std::string marker = guest.userName == event.organizerName ? " (organizer)" : "";
        result.lines.push_back(guest.userName + marker + " - " + toString(guest.rsvpStatus));
    }
    result.lines.push_back(response::kEnd);
    return publish(session, std::move(result));
}
