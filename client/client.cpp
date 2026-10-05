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

void closeSocket(int socketDescriptor) {
#ifdef _WIN32
    closesocket(socketDescriptor);
#else
    close(socketDescriptor);
#endif
}

int main() {
    string command;

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

    #ifdef _WIN32
        WSACleanup();
    #endif
    return 0;
}
