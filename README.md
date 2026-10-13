# PartyOn – Phase 2

**CIS 527 – Computer Networks · Socket Programming Project · Fall 2026**
University of Michigan–Dearborn · Dr. Jinhua Guo · Group 4

| Member | Area |
|---|---|
| Giovanna Ponce Cisternas | Client and user experience |
| LaNaysha Simms | Protocol, requirements, and testing |
| Andrew Luwaga | Server and application state |

---

## 1. What is PartyOn?

PartyOn is a client–server application for organizing small events such as birthdays, potlucks and family gatherings. Instead of scattering plans across group chats, everyone connects to one server that holds the current state of the event:

- **who is coming** (RSVP: `Yes`, `No`, `Maybe`)
- **who is bringing what** (items that guests claim and release)
- **when the event is** (date and time, which only the organizer can change)

When something changes, the server pushes a **notification** to the affected users, so nobody works from stale information.

Phase 2 delivers a working **one-client / one-server** version over TCP (*Level 1* in the protocol specification). Phase 3 will add concurrent clients.

---

## 2. Repository layout

```
PartyOn-Phase2/
├── Makefile                      # builds the server and the client
├── README.md                     # this file
├── client/
│   └── client.cpp                # command-line client (Giovanna)
├── server/
│   ├── main.cpp                  # startup, listen/accept loop, shutdown
│   ├── net.hpp                   # socket code for Windows and POSIX, send-all helper
│   ├── LineFramer.hpp/.cpp       # TCP stream → complete protocol lines
│   ├── Protocol.hpp/.cpp         # tokenizing, field/date/time/UTF-8 validation, response text
│   ├── AppState.hpp/.cpp         # Event, Guest, Item, ClientSession data model
│   ├── Dispatcher.hpp/.cpp       # command → handler, error-precedence rules
│   └── Logger.hpp/.cpp           # timestamped server log
├── tests/
│   ├── mock_server.cpp           # fake server used while building the client
│   ├── test_framer.cpp           # unit tests for message framing
│   ├── test_state.cpp            # unit tests for commands and response codes
│   ├── protocol_test.py          # conformance tests against a running server
│   └── Phase2-TestReport.md      # Phase 2 test report
└── docs/
    ├── UpdatedProtocolSpecification.md   # authoritative protocol (PartyOn v1.0)
    ├── PartyOn_Client_Server_Architecture_Diagram (1).png
    ├── Sequence-Diagram.png
    ├── server-log-sample.txt             # server log from three client sessions
    ├── two-machine-log.txt               # server log: macOS server, Windows client over VPN
    ├── Project - Phase 1.pdf             # requirements, data design, test plan
    └── Project - Phase 2.pdf             # Phase 2 responsibilities
```

> **Status:** server and client are merged and work together end to end. All unit and conformance tests pass on macOS. Still open: a Windows build check and a two-machine run (see `tests/Phase2-TestReport.md`).

---

## 3. Requirements

| | macOS / Linux | Windows |
|---|---|---|
| Compiler | `g++` or `clang++` with C++17 | MinGW-w64 `g++` with C++17 |
| Build tool | `make` | `mingw32-make` |
| Libraries | none (POSIX sockets, pthreads) | Winsock2 (`-lws2_32`, linked by the Makefile) |

There are no third-party dependencies.

**Windows note:** the Makefile uses `mkdir -p`, `rm -rf` and a shell `for` loop, so run it from **MSYS2** or **Git Bash** with MinGW-w64, not plain `cmd.exe`. Windows behaves differently in one way: Ctrl+C stops the server immediately only while it is waiting for a client. If a client is connected, the server exits after that client disconnects.

---

## 4. Build

From the repository root:

```bash
make            # builds bin/partyon-server and bin/partyon-client
make server     # server only
make client     # client only
make test       # builds and runs the unit tests
make clean      # removes bin/ and object files
```

---

## 5. Running the server

```bash
./bin/partyon-server            # listens on the default port 5527
./bin/partyon-server 6000       # listens on a different port
```

On startup the server prints something like:

```
[2026-10-11 14:02:10] [INFO] PartyOn server listening on 0.0.0.0:5527 (one client at a time)
[2026-10-11 14:02:10] [INFO] Waiting for a client...
```

- **Default port: `5527`.** The team moved off port 5000 because macOS uses it for the AirPlay Receiver, so the server often cannot bind there on a Mac.
- The server listens on all interfaces (`0.0.0.0`), so a client on another machine can connect using the server's IP address.
- **Phase 2 serves one client at a time.** Another client that connects while one is being served waits in the operating system's queue until the first one disconnects.
- When a client sends `QUIT` or drops, the server cleans up that session and accepts the next client. **The server keeps running.**
- To stop the server, press **Ctrl+C**. It closes its sockets and exits cleanly. The protocol has no shutdown command.

### Server log

Every connection, command, response code and disconnect goes both to the terminal and to `server.log` in the directory you started the server from:

```
[2026-10-11 14:02:31] [INFO] Client connected from 127.0.0.1:54012 (conn 1)
[2026-10-11 14:02:35] [INFO] conn 1 RECV "CREATE_EVENT Party1 Andrew 10/31/2026 07:00PM"
[2026-10-11 14:02:35] [INFO] conn 1 SEND 201 Created Event (+1 line)
[2026-10-11 14:02:38] [INFO] conn 1 RECV "CLAIM_ITEM Party1 Chips"
[2026-10-11 14:02:38] [INFO] conn 1 SEND 200 OK
[2026-10-11 14:02:38] [INFO] conn 1 NOTIFY 210 Party1 ITEM_CLAIMED Chips claimed by Andrew
[2026-10-11 14:02:41] [INFO] conn 1 RECV "RSVP Party1 Purple"
[2026-10-11 14:02:41] [WARN] conn 1 SEND 300 Message Format Error (invalid RSVP status (expected Yes, No, or Maybe) in position 2)
[2026-10-11 14:03:02] [INFO] conn 1 RECV "QUIT"
[2026-10-11 14:03:02] [INFO] conn 1 disconnected (client sent QUIT); session removed with 1 identity
[2026-10-11 14:03:02] [INFO] Waiting for a client...
```

---

## 6. Running the client

```bash
./bin/partyon-client
```

The client asks where the server is:

```
Server Host/IP: 127.0.0.1
Server Port: 5527
```

- For a server on the same machine, use `127.0.0.1` or `localhost`.
- For a server on another machine, use that machine's IP address and make sure its firewall allows TCP port 5527.

After connecting, the client shows the PartyOn banner and the command menu. Type a command at the `PartyOn>` prompt. Notifications from the server can arrive at any time and are shown with a `[Notification]` prefix.

**Building only the client on Windows without `make`:** if MinGW-w64 `g++` is installed, run these one at a time in PowerShell:

```powershell
g++ -std=c++17 -O2 client/client.cpp -o partyon-client.exe -lws2_32 -pthread
.\partyon-client.exe
```

### Server and client on different machines

If both machines are on the same Wi-Fi, use the server's local IP. If they're on **different networks**, connect both to the **school VPN (GlobalProtect)**; we tested this setup. Then:

1. **Server machine: find your VPN address.**
   - macOS: `ifconfig | awk '/^[a-z]/{iface=$1} /inet /{print iface, $2}'` and look for the `141.215.x.x` line (usually a `utun` interface)
   - Windows: `Get-NetIPAddress -AddressFamily IPv4 | Format-Table InterfaceAlias, IPAddress`
2. **Server machine: allow the server through the firewall.** On macOS the firewall blocks our unsigned binary until you add a rule. Repeat this after each rebuild:
   ```bash
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$(pwd)/bin/partyon-server"
   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$(pwd)/bin/partyon-server"
   ```
   Run it from the repository root. On Windows, click **Allow** when the firewall prompt appears.
3. **Start the server**, then **check reachability from the client machine**:
   - Windows: `Test-NetConnection -ComputerName <server-VPN-IP> -Port 5527` should print `TcpTestSucceeded : True`
   - macOS/Linux: `nc -vz <server-VPN-IP> 5527` should report "succeeded"
4. **Run the client** with the server's VPN IP and port `5527`, or run `python3 tests/protocol_test.py <server-VPN-IP> 5527`.

The server log shows each client's VPN address. A sample run is in `docs/two-machine-log.txt`.

---

## 7. Commands

All fields are separated by a single space. Event codes, usernames and item names are **case-sensitive**, cannot contain spaces, and are limited to 100 bytes.

| Command | Who can use it | What it does |
|---|---|---|
| `CREATE_EVENT <EventCode> <UserName> <MM/DD/YYYY> <HH:MMAM\|PM>` | anyone | Creates an event. You become its organizer. |
| `JOIN <EventCode> <UserName>` | anyone | Joins an existing event as a guest and shows its date and time. Joining again with the same name rejoins as that guest. |
| `RSVP <EventCode> <Yes\|No\|Maybe>` | guests | Sets your attendance. New guests start as `Maybe`. |
| `ITEMLIST <EventCode>` | guests | Lists items and who has claimed them. |
| `CLAIM_ITEM <EventCode> <ItemName>` | guests | Claims an item, creating it if it is new. |
| `RELEASE_ITEM <EventCode> <ItemName>` | guests | Releases an item you claimed, so someone else can bring it. |
| `WHO <EventCode>` | guests | Lists guests and their RSVP status. |
| `UPDATE_EVENT <EventCode> <MM/DD/YYYY> <HH:MMAM\|PM>` | organizer only | Changes the event date and time. |
| `DELETE_EVENT <EventCode>` | organizer only | Deletes the event. |
| `QUIT` | anyone | Disconnects. The server keeps running. |
| `HELP` | anyone (local) | Shows the command menu. Handled by the client and never sent to the server. |

Commands must be **UPPERCASE**. A connection can belong to several events, with a different username in each.

### Example session

```
PartyOn> CREATE_EVENT Party1 Andrew 10/31/2026 07:00PM
201 Created Event
EventCode: Party1

PartyOn> CLAIM_ITEM Party1 Chips
200 OK
[Notification] 210 Party1 ITEM_CLAIMED Chips claimed by Andrew

PartyOn> RSVP Party1 Yes
200 OK
[Notification] 210 Party1 RSVP_UPDATED Andrew status is now Yes

PartyOn> WHO Party1
200 OK
Andrew (organizer) - Yes
END

PartyOn> JOIN Party1 Andrew
410 Conflict

(from a new connection)
PartyOn> JOIN Party1 Sam
200 OK
Sam, you are now a guest of Party1 (10/31/2026 07:00PM).

PartyOn> ITEMLIST Party1
200 OK
Chips - claimed by Andrew
END

PartyOn> UPDATE_EVENT Party1 11/01/2026 06:30PM
200 OK
[Notification] 210 Party1 EVENT_UPDATED new event details: 11/01/2026 06:30PM

PartyOn> QUIT
Goodbye!
```

---

## 8. Response codes

| Code | Meaning | Typical cause |
|---|---|---|
| `200 OK` | Success | Most successful commands |
| `201 Created Event` | Event created | `CREATE_EVENT` |
| `202 Deleted Event` | Event deleted | `DELETE_EVENT` |
| `210 <Event> <TYPE> ...` | Notification | Something changed in an event you belong to |
| `300 Message Format Error` | Bad command | Unknown or empty command, missing or extra field, bad date/time/status, invalid UTF-8, line over 527 bytes |
| `401 Not Identified` | Not in that event | You haven't joined it, it never existed, or it was deleted |
| `402 Not Authorized` | Organizer only | A guest tried `UPDATE_EVENT` or `DELETE_EVENT` |
| `404 Not Found` | No such event | Only returned by `JOIN` |
| `410 Conflict` | State conflict | Event code taken, item already claimed, already joined this event |

When several errors apply, the server reports the first one in this order: **300 → 401 → 402 → 410** (`JOIN`: 300 → 404 → 410; `CREATE_EVENT`: 300 → 410).

Notification types: `ITEM_CLAIMED`, `ITEM_RELEASED`, `RSVP_UPDATED` (sent to the organizer), `EVENT_UPDATED`, `EVENT_DELETED`.

---

## 9. How it works

### Message framing

TCP is a byte stream, so one `recv()` may contain half a command or several commands at once. Both client and server:

1. append received bytes to a per-connection buffer,
2. process every complete line ending in `\n` and strip a trailing `\r`,
3. keep any partial line until the rest arrives,
4. if more than **527 bytes** arrive with no `\n`, treat the line as oversized. The server replies `300 Message Format Error`, and the receiver discards everything up to the next `\n`.
5. drop a partial line left in the buffer when the connection closes. It is never executed.

### Server structure

```
 TCP socket ──► LineFramer ──► Dispatcher ──► AppState (events, guests, items, sessions)
                                    │                │
                                    ▼                ▼
                                 Logger     Result { response lines, notifications }
                                                     │
                       socket ◄── write response block, then notifications
```

Command handlers never touch sockets. Each handler returns its **response lines** and a list of **notifications** with their recipients. The network layer writes the whole response first, then the notifications. This guarantees two rules from the spec:

- a response is never interrupted by a notification, and
- a client always gets the response to its own command before any notification that command caused.

The same design carries over to Phase 3: adding more client sessions and a lock per event does not change the handlers.

### Data model (from Phase 1 data design)

- `events: map<EventCode, Event>`
- `Event { eventCode, organizerName, date, time, guests[], items[] }`
- `Guest { userName, rsvpStatus (Yes|No|Maybe), isOrganizer }`
- `Item { itemName, claimedBy (empty = unclaimed) }`
- `ClientSession { connectionId, identities: map<EventCode, UserName> }`

---

## 10. Design decisions and changes from Phase 1

| Topic | Decision | Notes |
|---|---|---|
| Default port | **5527** (was 5000 in early testing) | Port 5000 is taken by AirPlay Receiver on macOS. 5527 nods to CIS 527 and the 527-byte line limit. |
| Command on a deleted event | **`401 Not Identified`** | Supersedes the Phase 1 data-design note that said `404`. Only `JOIN` returns `404`. |
| Empty line | **`300 Message Format Error`** | Supersedes Phase 1 test case TC-F07 ("ignored"). |
| Same username joins again | **Rejoin**, keeping their RSVP status | Supersedes FR-20 ("reject duplicate usernames"). There is no authentication. |
| Text of the 300 response | Always **`300 Message Format Error`** | The mock server's `300 Invalid Command` will be updated to match during integration. |
| JOIN reply shows date/time | Second line: `<User>, you are now a guest of <Event> (<Date> <Time>).` | Closes requirement FR-05 (view event date and time). Needs a matching update in the protocol spec. |
| Client oversized lines | Client will also enforce the 527-byte receive limit | Required by the spec's buffering rules for both sides. Scheduled for integration (Day 2). |
| Concurrency | Level 1 (one client at a time) | Level 2 with per-event locking is planned for Phase 3. |

---

## 11. Testing

```bash
make test                             # framing and state unit tests (no network needed)

./bin/partyon-server &                # conformance tests over real TCP
python3 tests/protocol_test.py        # optional: [host] [port]
```

`protocol_test.py` prints PASS or FAIL for each test ID (TC-F01 to F09, one per response code, and behavior checks). It works against any running server, including one on another machine.

**Automatic checks (GitHub Actions):** every push builds the project and runs the unit and conformance tests on **Linux and Windows** (`.github/workflows/build.yml`). See the repo's **Actions** tab, or the ✅/❌ next to a commit or PR. To start a run by hand, open Actions → *Build and test* → **Run workflow**.

Manual and integration testing follows the Phase 1 test plan (`docs/Project - Phase 1.pdf`, §9). Results are recorded in `tests/Phase2-TestReport.md`, and a sample server log is in `docs/server-log-sample.txt`. The framing tests cover one command per write, byte-by-byte delivery, several commands in one write, a split mid-token, a UTF-8 character split across reads, `\r\n` endings, a connection closed mid-line, and oversized lines.

To send raw protocol lines without the client:

```bash
nc 127.0.0.1 5527
CREATE_EVENT Party1 Andrew 10/31/2026 07:00PM
```

---

## 12. Troubleshooting

| Problem | Fix |
|---|---|
| `bind failed: Address already in use` | Another program, or an earlier server run, is using the port. Wait a few seconds, or run on another port (`./bin/partyon-server 6000`). |
| Client prints `Could not connect to the PartyOn server.` | Check that the server is running, that the host and port are right, and that the firewall allows the port. |
| Two machines: connection times out, nothing appears in the server log | Make sure **both** machines are connected to the VPN (the client must have a `141.215.x.x` address too), and add the macOS firewall rule from §6. |
| `socketfilterfw`: "The file path you specified does not exist" | Run the command from the repository root, or give the full path to `bin/partyon-server`. |
| `ifconfig \| grep utun` shows no address while on the VPN | Use the `awk` command in §6. It lists every interface with an IPv4 address. |
| Client connects but nothing happens | Phase 2 serves one client at a time, so another client may still be connected. Quit that client first. |
| Every command returns `300` | Commands are case-sensitive and uppercase, with single spaces and no extra fields. |
| `401 Not Identified` | `JOIN` or `CREATE_EVENT` the event on this connection first. Identities are lost when you reconnect. |

---

## 13. Roadmap

- **Phase 2 (this submission):** single client, full command set, framing, validation, logging, clean disconnects.
- **Phase 3:** multiple concurrent clients, per-event locks, notifications sent to every affected connection, race and resilience testing, and possibly saving events to a JSON file.
