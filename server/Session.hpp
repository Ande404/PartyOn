#pragma once

// One client connection (protocol spec v2, Sessions and Slow Clients).
//
// Output never goes straight to the socket from a command: it is queued, and
// a writer thread per session sends it. Event locks are therefore never held
// during a socket write, so a client that stops reading cannot slow down
// anyone else. If a session's queue grows past the line limit, the session is
// ended.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "AppState.hpp"
#include "net.hpp"

class Session {
public:
    Session(SessionId id, net::SocketType socket, std::string peer, std::size_t maxQueuedLines);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    SessionId id() const { return id_; }
    const std::string& peer() const { return peer_; }
    net::SocketType socket() const { return socket_; }

    // Starts the writer thread
    void start();

    // Queues complete lines for sending. Returns false if the session is
    // closed, or closes it now because its queue is over the limit.
    bool enqueue(const std::string& block);

    // Ends the session; the first reason given is kept. With flushPending the
    // writer sends what is already queued first (used for QUIT); otherwise
    // pending output is discarded and the socket is shut down at once.
    void close(const std::string& reason, bool flushPending = false);

    bool isClosed() const;
    std::string closeReason() const;

    // Called once by the reader thread when it is done: waits for the writer
    // to finish, then closes the socket
    void finish();

private:
    void writerLoop();

    const SessionId id_;
    const net::SocketType socket_;
    const std::string peer_;
    const std::size_t maxQueuedLines_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    std::size_t queuedLines_ = 0;
    bool closed_ = false;
    bool flushPending_ = false;
    std::string closeReason_;

    std::thread writer_;
};

// All live sessions, so notifications can find their recipients and shutdown
// can close everyone
class SessionRegistry {
public:
    void add(const std::shared_ptr<Session>& session);

    // Removes the session. onRemoved runs under the registry lock with the
    // number of sessions left, so its count is exact and it finishes before
    // waitUntilEmpty() can return.
    void remove(SessionId id, const std::function<void(std::size_t remaining)>& onRemoved = nullptr);
    std::shared_ptr<Session> find(SessionId id) const;
    std::size_t size() const;

    void closeAll(const std::string& reason);

    // Returns true if every session was removed before the timeout
    bool waitUntilEmpty(int timeoutMs);

private:
    mutable std::mutex mutex_;
    std::condition_variable emptied_;
    std::map<SessionId, std::shared_ptr<Session>> sessions_;
};
