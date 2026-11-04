#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

using namespace std;
mutex outputMutex;

enum class ResponseShape {
    SingleLine,
    TwoLines,
    UntilEnd
};

mutex responseMutex;
condition_variable responseCv;

bool waitingForResponse = false;
bool responseStarted = false;
bool responseComplete = false;

int responseLinesSeen = 0;

ResponseShape expectedResponseShape = ResponseShape::SingleLine;

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

void shutdownSocket(SocketType socketDescriptor) {
#ifdef _WIN32
    shutdown(socketDescriptor, SD_BOTH);
#else
    shutdown(socketDescriptor, SHUT_RDWR);
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

bool isWaitingForResponse() {
    lock_guard<mutex> lock(responseMutex);
    return waitingForResponse;
}


void displayServerLine(
    const string& line,
    bool isNotification
) {
    lock_guard<mutex> lock(outputMutex);

    if (isNotification) {
        cout << endl;
        cout << "[Notification] "
             << line << endl;
    }
    else {
        cout << endl;
        cout << line << endl;
    }
}



ResponseShape responseShapeForCommand(const string& command) {
    string commandName = command.substr(0, command.find(' '));

    if (commandName == "JOIN" ||
        commandName == "CREATE_EVENT") {
        return ResponseShape::TwoLines;
    }

    if (commandName == "ITEMLIST" ||
        commandName == "WHO") {
        return ResponseShape::UntilEnd;
    }

    return ResponseShape::SingleLine;
}

void beginResponseWait(const string& command) {
    lock_guard<mutex> lock(responseMutex);

    expectedResponseShape = responseShapeForCommand(command);

    waitingForResponse = true;
    responseStarted = false;
    responseComplete = false;
    responseLinesSeen = 0;
}

bool lineIsNotification(const string& line) {
    lock_guard<mutex> lock(responseMutex);

    // If we are already inside a multi-line response,
    // treat the line as part of that response.
    if (waitingForResponse && responseStarted) {
        return false;
    }

    return line.rfind("210 ", 0) == 0;
}

void recordResponseLine(const string& line) {
    lock_guard<mutex> lock(responseMutex);

    if (!waitingForResponse) {
        return;
    }

    if (!responseStarted) {
        responseStarted = true;
        responseLinesSeen = 1;

        // 3xx and 4xx responses are one-line errors.
        if (!line.empty() &&
            (line[0] == '3' || line[0] == '4')) {

            responseComplete = true;
        }
        else if (expectedResponseShape ==
                 ResponseShape::SingleLine) {

            responseComplete = true;
        }
    }
    else {
        ++responseLinesSeen;

        if (expectedResponseShape ==
                ResponseShape::TwoLines &&
            responseLinesSeen >= 2) {

            responseComplete = true;
        }
        else if (expectedResponseShape ==
                     ResponseShape::UntilEnd &&
                 line == "END") {

            responseComplete = true;
        }
    }

    if (responseComplete) {
        waitingForResponse = false;
        responseCv.notify_all();
    }
}


// Protocol limit for one line, including the terminating '\n'
const size_t MAX_MESSAGE_BYTES = 527;

void displayOversizedWarning() {
    lock_guard<mutex> lock(outputMutex);

    cout << endl;
    cout << "[Warning] Discarded a server message longer than "
         << MAX_MESSAGE_BYTES << " bytes." << endl;
}

// Receives complete protocol lines from the server
void receiveLoop(SocketType socketDescriptor, atomic<bool>& running) {
    char buffer[1024];
    string receiveBuffer;

    // True while skipping an oversized line up to its '\n'
    bool discardingOversizedLine = false;

    while (running) {
        int bytesReceived = recv(
            socketDescriptor,
            buffer,
            sizeof(buffer),
            0
        );

        if (bytesReceived <= 0) {
            if (running) {
                lock_guard<mutex> lock(outputMutex);

                cout << endl;
                cout << "Connection closed by server." << endl;
            }

            running = false;

            {
                lock_guard<mutex> lock(responseMutex);
                responseComplete = true;
            }

            responseCv.notify_all();

            break;
        }

        // Add newly received bytes to the saved buffer
        receiveBuffer.append(buffer, bytesReceived);

        size_t newlinePosition;

        // Process every complete line currently available
        while ((newlinePosition = receiveBuffer.find('\n')) != string::npos) {

            string line = receiveBuffer.substr(0, newlinePosition);

            receiveBuffer.erase(0, newlinePosition + 1);

            // Rest of an oversized line that was already reported
            if (discardingOversizedLine) {
                discardingOversizedLine = false;
                continue;
            }

            // Line plus its '\n' is over the protocol limit
            if (line.size() + 1 > MAX_MESSAGE_BYTES) {
                displayOversizedWarning();
                continue;
            }

            // Accept CRLF but remove the optional '\r'
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            bool isNotification =
                lineIsNotification(line);

            displayServerLine(
                line,
                isNotification
            );

            if (!isNotification) {
                recordResponseLine(line);
            }
        }

        // More than 527 bytes with no '\n' is an oversized line:
        // drop it and keep dropping bytes until the next '\n'
        if (discardingOversizedLine) {
            receiveBuffer.clear();
        }
        else if (receiveBuffer.size() > MAX_MESSAGE_BYTES) {
            receiveBuffer.clear();
            discardingOversizedLine = true;
            displayOversizedWarning();
        }
    }
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

    atomic<bool> running(true);

    thread receiverThread(
        receiveLoop,
        clientSocket,
        ref(running)
    );

    while (running) {
        cout << "PartyOn> " << flush;

        if (!getline(cin, command)) {
            cout << endl;
            cout << "Input closed. Exiting PartyOn." << endl;
            break;
        }

        if (!running) {
            break;
        }

        // Protocol limit includes the terminating '\n'
        if (command.size() + 1 > MAX_MESSAGE_BYTES) {
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

            running = false;
            shutdownSocket(clientSocket);

            break;
        }

        beginResponseWait(command);

        if (!sendMessage(clientSocket, command)) {
            cout << "Connection to server was lost." << endl;

            running = false;
            shutdownSocket(clientSocket);

            break;
        }
        {
            unique_lock<mutex> lock(responseMutex);

            responseCv.wait(
                lock,
                [&running] {
                    return responseComplete || !running.load();
                }
            );
        }
    }

    running = false;

    shutdownSocket(clientSocket);

    if (receiverThread.joinable()) {
        receiverThread.join();
    }

    closeSocket(clientSocket);

    #ifdef _WIN32
        WSACleanup();
    #endif

    return 0;
}
