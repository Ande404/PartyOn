// PartyOn server, Phase 3: many clients at once (protocol spec v2).
//
// Usage: partyon-server [port] [--data-dir DIR]
//   port        TCP port to listen on (default 5527)
//   --data-dir  where events are saved (default ./data)
//
// Threads: the main thread accepts connections. Each connection gets a reader
// thread (framing, parsing, applying commands) and a writer thread (sending
// its queued output; see Session.hpp). Shared state lives in AppState, which
// locks per event.

// Winsock headers must come before anything that may include <windows.h>
#include "net.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
    #include <signal.h>
    #include <sys/select.h>
#endif

#include "AppState.hpp"
#include "Dispatcher.hpp"
#include "EventStore.hpp"
#include "LineFramer.hpp"
#include "Logger.hpp"
#include "Protocol.hpp"
#include "Session.hpp"

namespace {

const char* kLogPath = "server.log";
const char* kDefaultDataDir = "data";

// Lines a session may have queued before it is treated as a stalled client
constexpr std::size_t kMaxQueuedLines = 1000;

// How often the accept loop checks for a shutdown request
constexpr long kAcceptPollMicros = 500 * 1000;

// How long shutdown waits for sessions to finish
constexpr int kShutdownWaitMs = 5000;

// Kernel send buffer per client. Without a cap, Linux grows it to several MB,
// so a client that stops reading could absorb tens of thousands of lines
// before the output-queue limit noticed. Capping it keeps the slow-client
// limit (and the memory one stalled client can hold) the same on every OS.
constexpr int kSocketSendBufferBytes = 64 * 1024;

std::atomic<bool> g_stopRequested{false};

void onStopSignal(int) {
    g_stopRequested = true;
}

void installSignalHandlers() {
#ifdef _WIN32
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
#else
    struct sigaction action {};
    action.sa_handler = onStopSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    // A client that disconnects mid-send must not kill the server
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

struct Options {
    std::uint16_t port = protocol::kDefaultPort;
    std::string dataDir = kDefaultDataDir;
};

bool parseArgs(int argc, char* argv[], Options& options) {
    bool havePort = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            options.dataDir = argv[++i];
        } else if (!havePort && !arg.empty() && arg[0] != '-') {
            char* end = nullptr;
            const long value = std::strtol(arg.c_str(), &end, 10);
            if (*end != '\0' || value < 1 || value > 65535) {
                return false;
            }
            options.port = static_cast<std::uint16_t>(value);
            havePort = true;
        } else {
            return false;
        }
    }
    return true;
}

net::SocketType createListener(std::uint16_t port) {
    net::SocketType listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == net::INVALID_SOCKET_VALUE) {
        logger::error("socket() failed: " + net::lastErrorString());
        return net::INVALID_SOCKET_VALUE;
    }

#ifndef _WIN32
    // Lets the server restart right away instead of waiting out TIME_WAIT
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        logger::error("bind to port " + std::to_string(port) + " failed: " + net::lastErrorString());
        net::closeSocket(listener);
        return net::INVALID_SOCKET_VALUE;
    }

    if (listen(listener, SOMAXCONN) != 0) {
        logger::error("listen() failed: " + net::lastErrorString());
        net::closeSocket(listener);
        return net::INVALID_SOCKET_VALUE;
    }

    return listener;
}

std::string describePeer(const sockaddr_in& peer) {
    char host[INET_ADDRSTRLEN] = "unknown";
    inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
    return std::string(host) + ":" + std::to_string(ntohs(peer.sin_port));
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string block;
    for (const std::string& line : lines) {
        block += line + "\n";
    }
    return block;
}

// One log line per command: session, command, event, result, fan-out count
void logCommand(const std::string& tag, const std::string& rawLine, bool oversized, const Result& result) {
    std::string entry = tag + " ";
    if (!result.command.empty()) {
        entry += result.command + (result.eventCode.empty() ? "" : " " + result.eventCode);
    } else if (oversized) {
        entry += "<line over " + std::to_string(protocol::kMaxLineBytes) + " bytes>";
    } else {
        entry += "rejected \"" + logger::escape(rawLine, !protocol::isValidUtf8(rawLine)) + "\"";
    }

    if (result.lines.empty()) {
        entry += " -> no response";
    } else {
        entry += " -> " + result.lines.front();
        if (result.lines.size() > 1) {
            const std::size_t extra = result.lines.size() - 1;
            entry += " (+" + std::to_string(extra) + (extra == 1 ? " line)" : " lines)");
        }
    }

    // Error lines already carry their reason; success notes (e.g. "rejoined") do not
    const char status = result.lines.empty() ? '2' : result.lines.front()[0];
    if (!result.note.empty() && status != '3' && status != '4') {
        entry += " [" + result.note + "]";
    }
    if (!result.notifications.empty()) {
        const std::size_t count = result.notifications.size();
        entry += "; notified " + std::to_string(count) + (count == 1 ? " connection" : " connections");
    }

    if (status == '3' || status == '4') {
        logger::warn(entry);
    } else {
        logger::info(entry);
    }
}

// Reader thread for one connection
void serveClient(std::shared_ptr<Session> session, AppState& state, Dispatcher& dispatcher,
                 SessionRegistry& registry) {
    const SessionId id = session->id();
    const std::string tag = "session " + std::to_string(id);

    LineFramer framer;
    char buffer[1024];
    std::string reason;

    while (reason.empty()) {
        const int received = static_cast<int>(recv(session->socket(), buffer, sizeof(buffer), 0));

        if (received == 0) {
            reason = "client closed the connection";
            break;
        }
        if (received < 0) {
            if (session->isClosed()) {
                break;  // closed elsewhere (shutdown, slow client); keep that reason
            }
            if (net::wasInterrupted()) {
                continue;
            }
            reason = "receive failed: " + net::lastErrorString();
            break;
        }

        for (const LineFramer::Line& line : framer.feed(buffer, static_cast<std::size_t>(received))) {
            Result result = line.oversized ? Dispatcher::oversizedLine() : dispatcher.handle(id, line.text);
            logCommand(tag, line.text, line.oversized, result);

            // Results produced under an event lock were already queued by AppState
            if (!result.delivered && !result.lines.empty()) {
                session->enqueue(joinLines(result.lines));
            }
            if (result.closeConnection) {
                reason = result.note;
                session->close(reason, /*flushPending=*/true);
                break;
            }
            if (session->isClosed()) {
                break;
            }
        }
    }

    // A partial line left in the framer is dropped with it
    session->close(reason.empty() ? "connection closed" : reason);
    const std::string finalReason = session->closeReason();
    const std::size_t identities = state.closeSession(id);
    session->finish();

    // Logged inside remove(): the count is exact, and shutdown cannot log
    // "Server stopped" before this line
    registry.remove(id, [&](std::size_t remaining) {
        logger::info(tag + " disconnected (" + finalReason + "); removed from " + std::to_string(identities) +
                     (identities == 1 ? " event" : " events") + "; " + std::to_string(remaining) +
                     " session(s) still connected");
    });
}

// Waits up to kAcceptPollMicros for a connection, so the stop flag is checked regularly
bool waitForConnection(net::SocketType listener) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{};
    timeout.tv_usec = kAcceptPollMicros;
    return select(static_cast<int>(listener + 1), &readable, nullptr, nullptr, &timeout) > 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parseArgs(argc, argv, options)) {
        std::cerr << "Usage: " << argv[0] << " [port] [--data-dir DIR]\n"
                  << "  port        1-65535, default " << protocol::kDefaultPort << "\n"
                  << "  --data-dir  where events are saved, default ./" << kDefaultDataDir << std::endl;
        return 1;
    }
    const std::uint16_t port = options.port;

    if (!logger::init(kLogPath)) {
        std::cerr << "Warning: could not open " << kLogPath << "; logging to the terminal only." << std::endl;
    }

    if (!net::startup()) {
        logger::error("Winsock initialization failed");
        return 1;
    }

    installSignalHandlers();

    const net::SocketType listener = createListener(port);
    if (listener == net::INVALID_SOCKET_VALUE) {
        net::cleanup();
        return 1;
    }

    // Deliberately never freed: detached reader threads may still be using
    // them if shutdown times out, and the process is exiting anyway
    auto* state = new AppState();
    auto* dispatcher = new Dispatcher(*state);
    auto* registry = new SessionRegistry();
    auto* store = new EventStore(options.dataDir);

    std::string storeError;
    if (!store->open(storeError)) {
        logger::error(storeError);
        net::closeSocket(listener);
        net::cleanup();
        return 1;
    }
    std::vector<std::string> warnings;
    std::size_t restored = 0;
    for (const Event& event : store->loadAll(warnings)) {
        restored += state->restore(event) ? 1 : 0;
    }
    for (const std::string& warning : warnings) {
        logger::warn("data: " + warning);
    }
    logger::info("Loaded " + std::to_string(restored) + (restored == 1 ? " event" : " events") + " from " +
                 store->directory().string());
    state->setStore(store);

    state->setSink([registry](SessionId recipient, const std::string& block) {
        if (auto session = registry->find(recipient)) {
            session->enqueue(block);
        }
    });

    logger::info("PartyOn server listening on 0.0.0.0:" + std::to_string(port) + " (multiple clients, " +
                 std::to_string(kMaxQueuedLines) + "-line output limit per client)");

    while (!g_stopRequested) {
        if (!waitForConnection(listener)) {
            continue;
        }

        sockaddr_in peer{};
        net::SockLen peerLength = sizeof(peer);
        const net::SocketType client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (client == net::INVALID_SOCKET_VALUE) {
            if (!net::wasInterrupted() && !g_stopRequested) {
                logger::error("accept() failed: " + net::lastErrorString());
            }
            continue;
        }

        int sendBuffer = kSocketSendBufferBytes;
        setsockopt(client, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBuffer), sizeof(sendBuffer));

        const SessionId id = state->openSession();
        auto session = std::make_shared<Session>(id, client, describePeer(peer), kMaxQueuedLines);
        registry->add(session);
        session->start();
        logger::info("session " + std::to_string(id) + " connected from " + session->peer() + "; " +
                     std::to_string(registry->size()) + " session(s) connected");

        std::thread(serveClient, session, std::ref(*state), std::ref(*dispatcher), std::ref(*registry)).detach();
    }

    logger::info("Shutting down: no new connections; closing " + std::to_string(registry->size()) + " session(s)");
    net::closeSocket(listener);
    registry->closeAll("server shutting down");
    if (!registry->waitUntilEmpty(kShutdownWaitMs)) {
        logger::warn("Some sessions did not finish within " + std::to_string(kShutdownWaitMs) + " ms");
    }
    logger::info("Server stopped");
    net::cleanup();
    return 0;
}
