#include <iostream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

using namespace std;

#ifdef _WIN32
using SocketType = SOCKET;
const SocketType INVALID_SOCKET_VALUE = INVALID_SOCKET;
#else
using SocketType = int;
const SocketType INVALID_SOCKET_VALUE = -1;
#endif

void closeSocket(SocketType socketDescriptor) {
#ifdef _WIN32
    closesocket(socketDescriptor);
#else
    close(socketDescriptor);
#endif
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "Failed to initialize Winsock." << endl;
        return 1;
    }
#endif

    SocketType serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (serverSocket == INVALID_SOCKET_VALUE) {
        cerr << "Could not create server socket." << endl;

#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(5000);
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(
            serverSocket,
            reinterpret_cast<sockaddr*>(&serverAddress),
            sizeof(serverAddress)
        ) != 0) {

        cerr << "Could not bind to port 5000." << endl;
        closeSocket(serverSocket);

#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (listen(serverSocket, 1) != 0) {
        cerr << "Could not listen on port 5000." << endl;
        closeSocket(serverSocket);

#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    cout << "Mock PartyOn server listening on localhost:5000..." << endl;
    cout << "Waiting for a client..." << endl;

    SocketType clientSocket = accept(serverSocket, nullptr, nullptr);

    if (clientSocket == INVALID_SOCKET_VALUE) {
        cerr << "Could not accept client connection." << endl;
        closeSocket(serverSocket);

#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    cout << "Client connected!" << endl;

    char buffer[1024];

    while (true) {
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived <= 0) {
            cout << "Client disconnected." << endl;
            break;
        }

        buffer[bytesReceived] = '\0';

        cout << "Received: " << buffer << endl;
    }

    closeSocket(clientSocket);
    closeSocket(serverSocket);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
