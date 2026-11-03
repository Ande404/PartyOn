#include "Session.hpp"

#include <algorithm>
#include <chrono>

Session::Session(SessionId id, net::SocketType socket, std::string peer, std::size_t maxQueuedLines)
    : id_(id), socket_(socket), peer_(std::move(peer)), maxQueuedLines_(maxQueuedLines) {}

Session::~Session() {
    if (writer_.joinable()) {
        close("session destroyed");
        writer_.join();
    }
}

void Session::start() {
    writer_ = std::thread(&Session::writerLoop, this);
}

bool Session::enqueue(const std::string& block) {
    const std::size_t lines = static_cast<std::size_t>(std::count(block.begin(), block.end(), '\n'));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (queuedLines_ + lines <= maxQueuedLines_) {
            queue_.push_back(block);
            queuedLines_ += lines;
            wake_.notify_one();
            return true;
        }
    }
    close("output queue over " + std::to_string(maxQueuedLines_) + " lines (client not reading)");
    return false;
}

void Session::close(const std::string& reason, bool flushPending) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        flushPending_ = flushPending;
        closeReason_ = reason;
        if (!flushPending) {
            queue_.clear();
            queuedLines_ = 0;
        }
        wake_.notify_one();
    }
    if (!flushPending) {
        // Unblocks this session's recv() and send() calls
        net::shutdownSocket(socket_);
    }
}

bool Session::isClosed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::string Session::closeReason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closeReason_;
}

void Session::finish() {
    if (writer_.joinable()) {
        writer_.join();
    }
    net::shutdownSocket(socket_);
    net::closeSocket(socket_);
}

void Session::writerLoop() {
    while (true) {
        std::string batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return closed_ || !queue_.empty(); });
            if (queue_.empty() || (closed_ && !flushPending_)) {
                return;
            }
            // Send everything queued so far in one write; each queued block
            // is a whole response or notification, so blocks never interleave
            while (!queue_.empty()) {
                batch += queue_.front();
                queue_.pop_front();
            }
            queuedLines_ = 0;
        }

        if (!net::sendAll(socket_, batch)) {
            close("send failed: " + net::lastErrorString());
            return;
        }
    }
}

void SessionRegistry::add(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session->id()] = session;
}

void SessionRegistry::remove(SessionId id, const std::function<void(std::size_t remaining)>& onRemoved) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
    if (onRemoved) {
        onRemoved(sessions_.size());
    }
    if (sessions_.empty()) {
        emptied_.notify_all();
    }
}

std::shared_ptr<Session> SessionRegistry::find(SessionId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void SessionRegistry::closeAll(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, session] : sessions_) {
        session->close(reason);
    }
}

bool SessionRegistry::waitUntilEmpty(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return emptied_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return sessions_.empty(); });
}
