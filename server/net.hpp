#pragma once

// Socket helpers that work on both Windows (Winsock2) and POSIX systems.

#include <string>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    // inet_ntop needs Windows Vista or newer; some MinGW setups default lower
    #if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
        #undef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #ifdef _MSC_VER
        #pragma comment(lib, "Ws2_32.lib")
    #endif
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <cstring>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace net {

#ifdef _WIN32
using SocketType = SOCKET;
const SocketType INVALID_SOCKET_VALUE = INVALID_SOCKET;
using SockLen = int;
#else
using SocketType = int;
const SocketType INVALID_SOCKET_VALUE = -1;
using SockLen = socklen_t;
#endif

inline bool startup() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

inline void cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

inline void closeSocket(SocketType socketDescriptor) {
#ifdef _WIN32
    closesocket(socketDescriptor);
#else
    close(socketDescriptor);
#endif
}

inline void shutdownSocket(SocketType socketDescriptor) {
#ifdef _WIN32
    shutdown(socketDescriptor, SD_BOTH);
#else
    shutdown(socketDescriptor, SHUT_RDWR);
#endif
}

// True when the last socket call failed only because a signal interrupted it
inline bool wasInterrupted() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

inline std::string lastErrorString() {
#ifdef _WIN32
    return "WSA error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

// Sends every byte of `data`, retrying partial sends
inline bool sendAll(SocketType socketDescriptor, const std::string& data) {
    std::size_t totalSent = 0;
    while (totalSent < data.size()) {
        const int sent = static_cast<int>(
            send(socketDescriptor, data.data() + totalSent, static_cast<int>(data.size() - totalSent), 0));
        if (sent < 0 && wasInterrupted()) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        totalSent += static_cast<std::size_t>(sent);
    }
    return true;
}

}  // namespace net
