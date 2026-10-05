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

    cout << "================================" << endl;
    cout << "           PartyOn" << endl;
    cout << "================================" << endl;
    cout << "Welcome to PartyOn!" << endl;
    cout << "Type HELP to see available commands." << endl;
    cout << "Type QUIT to exit." << endl;
    cout << endl;


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


    while (true) {
        cout << "PartyOn> ";

        getline(cin, command);

        if (command == "QUIT") {
            cout << "Goodbye!" << endl;
            break;
        }

        if (command == "HELP") {
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
            cout << endl;

            continue;
        }

        if (command.empty()) {
            cout << "Please enter a command." << endl;
            continue;
        }

        cout << "You entered: " << command << endl;
    }

    closeSocket(clientSocket);

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}
