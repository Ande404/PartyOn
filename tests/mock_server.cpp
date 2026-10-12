#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <sstream>

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
    string receiveBuffer;
    bool clientRequestedQuit = false;

    while (!clientRequestedQuit) {
        int bytesReceived = recv(
            clientSocket,
            buffer,
            sizeof(buffer),
            0
        );

        if (bytesReceived <= 0) {
            cout << "Client disconnected." << endl;
            break;
        }

        // Save all bytes received from TCP
        receiveBuffer.append(buffer, bytesReceived);

        size_t newlinePosition;

        // Process every complete command ending in '\n'
        while ((newlinePosition = receiveBuffer.find('\n')) != string::npos) {
            string message = receiveBuffer.substr(0, newlinePosition);

            // Remove the processed command from the buffer
            receiveBuffer.erase(0, newlinePosition + 1);

            // Accept CRLF if received
            if (!message.empty() && message.back() == '\r') {
                message.pop_back();
            }

            cout << "Received: " << message << endl;

            // QUIT closes the connection without a response
            if (message == "QUIT") {
                cout << "Client requested disconnect." << endl;
                clientRequestedQuit = true;
                break;
            }

            // Empty command test
            if (message.empty()) {
                string response = "300 Invalid Command\n";

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );
            }

            // Temporary response used to test the PartyOn client
            // Test a multi-line WHO response
            else if (message == "WHO Party1") {
                string response =
                    "200 OK\n"
                    "User1 (organizer) - Yes\n"
                    "User2 - Maybe\n"
                    "END\n";

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );
            }

            // Fragmented TCP response test
            else if (message == "RSVP Party1 Yes") {
                string firstPart = "200 O";
                string secondPart = "K\n";

                send(
                    clientSocket,
                    firstPart.c_str(),
                    static_cast<int>(firstPart.size()),
                    0
                );

                this_thread::sleep_for(chrono::milliseconds(200));

                send(
                    clientSocket,
                    secondPart.c_str(),
                    static_cast<int>(secondPart.size()),
                    0
                );
            }

            // ITEM_CLAIMED notification
            else if (message == "CLAIM_ITEM Party1 Chips") {
                string response = "200 OK\n";

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );

                this_thread::sleep_for(chrono::milliseconds(500));

                string notification =
                    "210 Party1 ITEM_CLAIMED Chips claimed by User1\n";

                send(
                    clientSocket,
                    notification.c_str(),
                    static_cast<int>(notification.size()),
                    0
                );
            }

            // ITEM_RELEASED notification
            else if (message == "RELEASE_ITEM Party1 Chips") {
                string response = "200 OK\n";

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );

                this_thread::sleep_for(chrono::milliseconds(500));

                string notification =
                    "210 Party1 ITEM_RELEASED Chips is available to bring again\n";

                send(
                    clientSocket,
                    notification.c_str(),
                    static_cast<int>(notification.size()),
                    0
                );
            }

            // EVENT_UPDATED notification
            else if (message == "UPDATE_EVENT Party1 10/15/2026 06:00PM") {
                string response = "200 OK\n";

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );

                this_thread::sleep_for(chrono::milliseconds(500));

                string notification =
                    "210 Party1 EVENT_UPDATED new event details: 10/15/2026 06:00PM\n";

                send(
                    clientSocket,
                    notification.c_str(),
                    static_cast<int>(notification.size()),
                    0
                );
            }

            // RSVP_UPDATED notification
            else if (message == "RSVP Party1 No") {
                string response = "200 OK\n";

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );

                this_thread::sleep_for(chrono::milliseconds(500));

                string notification =
                    "210 Party1 RSVP_UPDATED User1 status is now No\n";

                send(
                    clientSocket,
                    notification.c_str(),
                    static_cast<int>(notification.size()),
                    0
                );
            }

            // EVENT_DELETED notification
            else if (message == "DELETE_EVENT Party1") {
                string response = "202 Deleted Event\n";

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );

                this_thread::sleep_for(chrono::milliseconds(500));

                string notification =
                    "210 Party1 EVENT_DELETED This event has been deleted\n";

                send(
                    clientSocket,
                    notification.c_str(),
                    static_cast<int>(notification.size()),
                    0
                );
            }

            
            // Generic response for other valid PartyOn commands
            else {
                istringstream input(message);
                string command;

                input >> command;

                bool validCommand =
                    command == "JOIN" ||
                    command == "CREATE_EVENT" ||
                    command == "DELETE_EVENT" ||
                    command == "RSVP" ||
                    command == "ITEMLIST" ||
                    command == "CLAIM_ITEM" ||
                    command == "RELEASE_ITEM" ||
                    command == "UPDATE_EVENT" ||
                    command == "WHO";

                string response;

                if (validCommand) {
                    response = "200 OK\n";
                }
                else {
                    response = "300 Invalid Command\n";
                }

                send(
                    clientSocket,
                    response.c_str(),
                    static_cast<int>(response.size()),
                    0
                );
            }
        }
    }

    closeSocket(clientSocket);
    closeSocket(serverSocket);

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}
