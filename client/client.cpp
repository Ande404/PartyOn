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
    #include <netdb.h>
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

SocketType connectToServer(const string& host, const string& port) {
    struct addrinfo hints {};
    struct addrinfo* result = nullptr;

    hints.ai_family = AF_UNSPEC;       // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;   // TCP
    hints.ai_protocol = IPPROTO_TCP;

    int status = getaddrinfo(
        host.c_str(),
        port.c_str(),
        &hints,
        &result
    );

    if (status != 0) {
        cerr << "Unable to resolve server address." << endl;
        return INVALID_SOCKET_VALUE;
    }

    SocketType clientSocket = INVALID_SOCKET_VALUE;

    for (struct addrinfo* address = result;
         address != nullptr;
         address = address->ai_next) {

        clientSocket = socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol
        );

        if (clientSocket == INVALID_SOCKET_VALUE) {
            continue;
        }

        if (connect(
                clientSocket,
                address->ai_addr,
                static_cast<int>(address->ai_addrlen)
            ) == 0) {

            break;
        }

        closeSocket(clientSocket);
        clientSocket = INVALID_SOCKET_VALUE;
    }

    freeaddrinfo(result);

    return clientSocket;
}

// Sends a complete protocol message terminated by '\n'
bool sendMessage(SocketType socketDescriptor, const string& message) {
    string data = message + "\n";

    int totalSent = 0;
    int messageLength = static_cast<int>(data.size());

    while (totalSent < messageLength) {
        int bytesSent = send(
            socketDescriptor,
            data.c_str() + totalSent,
            messageLength - totalSent,
            0
        );

        if (bytesSent <= 0) {
            cerr << "Failed to send message to server." << endl;
            return false;
        }

        totalSent += bytesSent;
    }

    return true;
}

void displayHelp() {
    cout << endl;
    cout << "Available commands:" << endl;
    cout << "JOIN" << endl;
    cout << "CREATE_EVENT" << endl;
    cout << "DELETE_EVENT" << endl;
    cout << "RSVP" << endl;
    cout << "ITEMLIST" << endl;
    cout << "CLAIM_ITEM" << endl;
    cout << "RELEASE_ITEM" << endl;
    cout << "UPDATE_EVENT" << endl;
    cout << "WHO" << endl;
    cout << "QUIT" << endl;
    cout << "HELP" << endl;
    cout << endl;
}

int main() {
    string command;

    string host;
    string port;

    #ifdef _WIN32
        WSADATA wsaData;

        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            cerr << "Failed to initialize Winsock." << endl;
            return 1;
        }
    #endif

    cout << "Server Host/IP: ";
    getline(cin, host);

    cout << "Server Port: ";
    getline(cin, port);

    cout << endl;
    cout << "Connecting to server..." << endl;

    SocketType clientSocket = connectToServer(host, port);

    if (clientSocket == INVALID_SOCKET_VALUE) {
        cerr << "Could not connect to the PartyOn server." << endl;

    #ifdef _WIN32
        WSACleanup();
    #endif

        return 1;
    }

    cout << "Connected to PartyOn server." << endl;
    cout << endl;

    cout << "================================" << endl;
    cout << "           PartyOn" << endl;
    cout << "================================" << endl;
    cout << "Welcome to PartyOn!" << endl;

    displayHelp();


    while (true) {
        cout << "PartyOn> ";

        if (!getline(cin, command)) {
            cout << endl;
            cout << "Input closed. Exiting PartyOn." << endl;
            break;
        }

        // Protocol limit includes the terminating '\n'
        if (command.size() + 1 > 527) {
            cout << "Message exceeds the 527-byte protocol limit." << endl;
            continue;
        }

        if (command == "HELP") {
            displayHelp();
            continue;
        }

        if (command == "QUIT") {
            sendMessage(clientSocket, command);
            cout << "Goodbye!" << endl;
            break;
        }

        if (!sendMessage(clientSocket, command)) {
            cout << "Connection to server was lost." << endl;
            break;
        }
    }

    closeSocket(clientSocket);

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}
