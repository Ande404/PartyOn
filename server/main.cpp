// PartyOn server, Phase 2 (protocol conformance Level 1: one client at a time).
//
// Usage: partyon-server [port]      (default port 5527)

// Winsock headers must come before anything that may include <windows.h>
#include "net.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef _WIN32
    #include <signal.h>
#endif

#include "AppState.hpp"
#include "Dispatcher.hpp"
#include "LineFramer.hpp"
#include "Logger.hpp"
#include "Protocol.hpp"

namespace {

const char* kLogPath = "server.log";

std::atomic<bool> g_stopRequested{false};
net::SocketType g_listenSocket = net::INVALID_SOCKET_VALUE;

void onStopSignal(int) {
    g_stopRequested = true;
#ifdef _WIN32
    // Signals do not interrupt accept() on Windows; closing the socket does
    net::closeSocket(g_listenSocket);
#endif
}

void installSignalHandlers() {
#ifdef _WIN32
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
#else
    // No SA_RESTART, so a blocked accept()/recv() returns EINTR on Ctrl+C
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

bool parsePort(int argc, char* argv[], std::uint16_t& port) {
    port = protocol::kDefaultPort;
    if (argc < 2) {
        return true;
    }
    char* end = nullptr;
    const long value = std::strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || value < 1 || value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
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

    if (listen(listener, 5) != 0) {
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

// Writes the response block in one piece, then this session's notifications,
// so a notification never lands inside a response
bool deliver(net::SocketType client, SessionId session, const std::string& tag, const Result& result) {
    if (!result.lines.empty()) {
        std::string block;
        for (const std::string& line : result.lines) {
            block += line + "\n";
        }

        std::string entry = tag + " SEND " + result.lines.front();
        if (result.lines.size() > 1) {
            const std::size_t extra = result.lines.size() - 1;
            entry += " (+" + std::to_string(extra) + (extra == 1 ? " line)" : " lines)");
        }
        if (!result.note.empty()) {
            entry += " (" + result.note + ")";
        }
        const char status = result.lines.front()[0];
        if (status == '3' || status == '4') {
            logger::warn(entry);
        } else {
            logger::info(entry);
        }

        if (!net::sendAll(client, block)) {
            return false;
        }
    }

    for (const Notification& notification : result.notifications) {
        // Level 1: only this connection exists, so every recipient is this session
        if (notification.recipient != session) {
            logger::warn(tag + " no connection for session " + std::to_string(notification.recipient) +
                         ", notification dropped: " + notification.line);
            continue;
        }
        logger::info(tag + " NOTIFY " + notification.line);
        if (!net::sendAll(client, notification.line + "\n")) {
            return false;
        }
    }

    return true;
}

void serveClient(net::SocketType client, const std::string& peer, AppState& state, Dispatcher& dispatcher) {
    const SessionId session = state.openSession();
    const std::string tag = "conn " + std::to_string(session);
    logger::info("Client connected from " + peer + " (" + tag + ")");

    LineFramer framer;
    char buffer[1024];
    bool open = true;
    std::string closeReason;

    while (open) {
        const int received = static_cast<int>(recv(client, buffer, sizeof(buffer), 0));

        if (received == 0) {
            closeReason = "client closed the connection";
            break;
        }
        if (received < 0) {
            if (g_stopRequested) {
                closeReason = "server shutting down";
                break;
            }
            if (net::wasInterrupted()) {
                continue;
            }
            closeReason = "receive failed: " + net::lastErrorString();
            break;
        }

        for (const LineFramer::Line& line : framer.feed(buffer, static_cast<std::size_t>(received))) {
            Result result;
            if (line.oversized) {
                logger::warn(tag + " RECV <line over " + std::to_string(protocol::kMaxLineBytes) +
                             " bytes, discarded>");
                result = Dispatcher::oversizedLine();
            } else {
                logger::info(tag + " RECV \"" + logger::escape(line.text, !protocol::isValidUtf8(line.text)) + "\"");
                result = dispatcher.handle(session, line.text);
            }

            if (!deliver(client, session, tag, result)) {
                closeReason = "send failed: " + net::lastErrorString();
                open = false;
                break;
            }
            if (result.closeConnection) {
                closeReason = result.note;
                open = false;
                break;
            }
        }
    }

    // Any partial line left in the framer is dropped with it
    const std::size_t identities = state.closeSession(session);
    logger::info(tag + " disconnected (" + closeReason + "); session removed with " + std::to_string(identities) +
                 (identities == 1 ? " identity" : " identities"));

    net::shutdownSocket(client);
    net::closeSocket(client);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::uint16_t port;
    if (!parsePort(argc, argv, port)) {
        std::cerr << "Usage: " << argv[0] << " [port]   (1-65535, default " << protocol::kDefaultPort << ")"
                  << std::endl;
        return 1;
    }

    if (!logger::init(kLogPath)) {
        std::cerr << "Warning: could not open " << kLogPath << "; logging to the terminal only." << std::endl;
    }

    if (!net::startup()) {
        logger::error("Winsock initialization failed");
        return 1;
    }

    installSignalHandlers();

    g_listenSocket = createListener(port);
    if (g_listenSocket == net::INVALID_SOCKET_VALUE) {
        net::cleanup();
        return 1;
    }

    logger::info("PartyOn server listening on 0.0.0.0:" + std::to_string(port) + " (one client at a time)");

    AppState state;
    Dispatcher dispatcher(state);

    while (!g_stopRequested) {
        logger::info("Waiting for a client...");

        sockaddr_in peer{};
        net::SockLen peerLength = sizeof(peer);
        const net::SocketType client = accept(g_listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLength);

        if (client == net::INVALID_SOCKET_VALUE) {
            if (g_stopRequested) {
                break;
            }
            if (!net::wasInterrupted()) {
                logger::error("accept() failed: " + net::lastErrorString());
            }
            continue;
        }

        serveClient(client, describePeer(peer), state, dispatcher);
    }

    logger::info("Shutting down");
#ifndef _WIN32
    net::closeSocket(g_listenSocket);
#endif
    net::cleanup();
    return 0;
}
