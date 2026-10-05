#include <iostream>
#include <string>

using namespace std;

int main() {
    string command;

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

    return 0;
}
