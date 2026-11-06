# PartyOn

**CIS 527  Computer Networks · Socket Programming Project · Fall 2026**
University of Michigan–Dearborn · Group 4

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

The server handles **many clients at once** over TCP, using the protocol in `docs/ProtocolSpecification-v2.md`. Each event's state is protected by its own lock, conflicting operations are resolved first come, first served, events are saved to disk so they survive restarts, and a failing or stalled client never affects the others. The key feature is handling **last-minute changes**: when a guest cancels, everything they were bringing is released at once and every guest is told what still needs covering.

---

## 2. Repository layout

```
.
├── Makefile                      # builds the server, client, and tests
├── README.md                     # this file
├── client/
│   └── client.cpp                # command-line client (Giovanna)
├── server/
│   ├── main.cpp                  # startup, accept loop, reader thread per client, shutdown
│   ├── Session.hpp/.cpp          # one connection: output queue, writer thread, slow-client limit
│   ├── AppState.hpp/.cpp         # events, guests, items; one lock per event
│   ├── EventStore.hpp/.cpp       # saves each event to disk (one file per event)
│   ├── Dispatcher.hpp/.cpp       # command → handler, validation, error reasons
│   ├── LineFramer.hpp/.cpp       # TCP stream → complete protocol lines
│   ├── Protocol.hpp/.cpp         # constants, field/date/time/UTF-8 validation, message text
│   ├── Logger.hpp/.cpp           # timestamped, thread-safe server log
│   └── net.hpp                   # socket code for Windows and POSIX
├── tests/
│   ├── test_framer.cpp           # unit: message framing
│   ├── test_state.cpp            # unit: commands, response codes, error reasons
│   ├── test_concurrency.cpp      # unit: many threads at once (also run under ThreadSanitizer)
│   ├── test_store.cpp            # unit: saving and loading events
│   ├── protocol_test.py          # live: protocol conformance
│   ├── concurrency_test.py       # live: simultaneous clients, conflicts, failures
│   ├── persistence_test.py       # live: crash/restart and Ctrl+C
│   ├── load_test.py              # live: throughput and latency for 10-200 clients
│   ├── mock_server.cpp           # fake server used while building the client
│   ├── TestUtil.hpp              # small assertion helpers for the C++ tests
│   └── Phase2-TestReport.md      # test report for the single-client milestone
├── docs/
│   ├── ProtocolSpecification-v2.md       # the protocol (PartyOn v2.0)
│   ├── UpdatedProtocolSpecification.md   # earlier single-client protocol (v1.0)
│   ├── PartyOn_Client_Server_Architecture_Diagram (1).png
│   ├── Sequence-Diagram.png
│   ├── screenshots/                      # screenshots of the application and CI
│   ├── server-log-sample.txt             # older log (single-client server, old format)
│   ├── two-machine-log.txt               # older log: macOS server, Windows client over VPN
│   ├── Project - Phase 1.pdf             # proposal: requirements, data design, test plan
│   └── Project - Phase 2.pdf             # team responsibilities
├── logs/
│   └── sample-server.log         # server log: 3 clients, conflict, cancellation, crash, restart
└── data/                         # saved events (created at runtime, not in git)
```

There is no `shared/` folder (the course's suggested layout lists it as optional). The client and server share only the protocol, which is defined in `docs/ProtocolSpecification-v2.md`; the client is a single self-contained file, so the two programs share no source code.

---

## 3. Requirements

| | macOS / Linux | Windows |
|---|---|---|
| Compiler | `g++` or `clang++` with C++17 | MinGW-w64 `g++` with C++17 |
| Build tool | `make` | `make` from MSYS2 (or Git Bash) |
| Libraries | none (POSIX sockets, pthreads) | Winsock2 (`-lws2_32`, linked by the Makefile) |

There are no third-party dependencies.

**Windows note:** the Makefile uses `mkdir -p`, `rm -rf` and a shell `for` loop, so run it from **MSYS2** or **Git Bash** with MinGW-w64, not plain `cmd.exe`. To build only the client without `make`, see §6.

---

## 4. Build

From the repository root:

```bash
make            # builds bin/partyon-server and bin/partyon-client
make server     # server only
make client     # client only
make test       # builds and runs the unit tests
make tsan       # runs the unit tests under ThreadSanitizer (data-race detector)
make clean      # removes bin/ and object files
```

---

## 5. Running the server

```bash
./bin/partyon-server                          # port 5527, events saved in ./data
./bin/partyon-server 6000                     # a different port
./bin/partyon-server 5527 --data-dir /tmp/po  # save events somewhere else
```

| Argument | Default | Meaning |
|---|---|---|
| `port` | `5527` | TCP port to listen on (1–65535) |
| `--data-dir DIR` | `data` | Folder where events are saved, one file per event. Created if missing |

The client takes no arguments; it asks for the server host and port when it starts.

On startup the server prints something like:

```
[2026-11-03 14:02:10] [INFO] PartyOn server listening on 0.0.0.0:5527 (multiple clients, 1000-line output limit per client)
```

- **Default port: `5527`.** The team moved off port 5000 because macOS uses it for the AirPlay Receiver, so the server often cannot bind there on a Mac.
- The server listens on all interfaces (`0.0.0.0`), so a client on another machine can connect using the server's IP address.
- **Many clients at once.** Every connection is served immediately, with no fixed maximum. Each connection is a *session* with its own ID in the log.
- When a client sends `QUIT`, drops, or sends bad input, only that session is affected. **The server and every other client keep running.**
- A client that stops reading is disconnected once **1000 lines** are waiting for it, so it can't hold up notifications to anyone else. Each client's operating-system send buffer is capped at 64 KB so this limit works the same on Linux, macOS, and Windows.
- To stop the server, press **Ctrl+C** (on Windows too). It stops accepting, closes every session, and exits. Clients see an ordinary disconnect.
- **Events survive restarts and crashes.** Every change is saved to `--data-dir` before the server replies. Restart the server with the same folder and every party, guest, RSVP, and item is back. Connections are not saved, so users `JOIN` again after a restart. To start fresh, delete the folder.

### Server log

Every connection, applied command (with its result and how many connections were notified), and disconnect (with the reason) goes both to the terminal and to `server.log` in the directory you started the server from:

```
[2026-11-03 14:02:31] [INFO] session 1 connected from 127.0.0.1:54012; 1 session(s) connected
[2026-11-03 14:02:33] [INFO] session 2 connected from 127.0.0.1:54013; 2 session(s) connected
[2026-11-03 14:02:35] [INFO] session 1 CREATE_EVENT Party1 -> 201 Created Event (+1 line)
[2026-11-03 14:02:37] [INFO] session 2 JOIN Party1 -> 200 OK (+1 line) [new guest]
[2026-11-03 14:02:40] [INFO] session 2 CLAIM_ITEM Party1 -> 200 OK; notified 2 connections
[2026-11-03 14:02:40] [WARN] session 1 CLAIM_ITEM Party1 -> 410 Conflict: item already claimed by Giovanna
[2026-11-03 14:02:41] [WARN] session 2 rejected "RSVP Party1 Purple" -> 300 Message Format Error: invalid RSVP status in field 2 (expected Yes, No, or Maybe)
[2026-11-03 14:03:02] [INFO] session 2 disconnected (client sent QUIT); removed from 1 event; 1 session(s) still connected
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
| `RSVP <EventCode> <Yes\|No\|Maybe>` | guests | Sets your attendance. New guests start as `Maybe`. **`No` also releases every item you claimed**, and everyone is notified. While your RSVP is `No` you can't claim items; change it to `Yes` or `Maybe` first. |
| `ITEMLIST <EventCode>` | guests | Lists items and who has claimed them. |
| `CLAIM_ITEM <EventCode> <ItemName>` | guests | Claims an item, creating it if it is new. Not allowed while your RSVP is `No`. |
| `RELEASE_ITEM <EventCode> <ItemName>` | guests | Releases an item you claimed, so someone else can bring it. |
| `WHO <EventCode>` | guests | Lists guests and their RSVP status. |
| `UPDATE_EVENT <EventCode> <MM/DD/YYYY> <HH:MMAM\|PM>` | organizer only | Changes the event date and time. |
| `DELETE_EVENT <EventCode>` | organizer only | Deletes the event. |
| `QUIT` | anyone | Disconnects. The server keeps running. |
| `HELP` | anyone (local) | Shows the command menu. Handled by the client and never sent to the server. |

Commands must be **UPPERCASE**. A connection can belong to several events, with a different username in each.

### Example interaction: three people, one party

This comes from a real run with three `partyon-client` windows connected to one server (the matching server log is `logs/sample-server.log`). Andrew organizes a graduation party; Giovanna and LaNaysha are guests. Lines a person typed follow `PartyOn>`, and everything else is from the server. Blank lines are removed.

**1. Andrew creates the party; the guests join**

```
Andrew>    CREATE_EVENT GradParty Andrew 12/12/2026 06:00PM
           201 Created Event
           EventCode: GradParty
Giovanna>  JOIN GradParty Giovanna
           200 OK
           Giovanna, you are now a guest of GradParty (12/12/2026 06:00PM).
LaNaysha>  JOIN GradParty LaNaysha
           200 OK
           LaNaysha, you are now a guest of GradParty (12/12/2026 06:00PM).
```

**2. Giovanna and LaNaysha both claim the cake at the same moment.** The server applies one claim first. Everyone, including the loser and Andrew, is told who won.

```
Giovanna>  CLAIM_ITEM GradParty Cake
           200 OK
           [Notification] 210 GradParty ITEM_CLAIMED Cake claimed by Giovanna
LaNaysha>  CLAIM_ITEM GradParty Cake
           [Notification] 210 GradParty ITEM_CLAIMED Cake claimed by Giovanna
           410 Conflict: item already claimed by Giovanna
Andrew     [Notification] 210 GradParty ITEM_CLAIMED Cake claimed by Giovanna
LaNaysha>  CLAIM_ITEM GradParty Chips
           200 OK
           [Notification] 210 GradParty ITEM_CLAIMED Chips claimed by LaNaysha
Giovanna>  CLAIM_ITEM GradParty Punch
           200 OK
           [Notification] 210 GradParty ITEM_CLAIMED Punch claimed by Giovanna
```

**3. A typo gets an error that says how to fix it**

```
LaNaysha>  RSVP GradParty Purple
           300 Message Format Error: invalid RSVP status in field 2 (expected Yes, No, or Maybe)
```

**4. The organizer moves the party; every guest is told**

```
Andrew>    UPDATE_EVENT GradParty 12/13/2026 07:00PM
           200 OK
           [Notification] 210 GradParty EVENT_UPDATED new event details: 12/13/2026 07:00PM
Giovanna   [Notification] 210 GradParty EVENT_UPDATED new event details: 12/13/2026 07:00PM
LaNaysha   [Notification] 210 GradParty EVENT_UPDATED new event details: 12/13/2026 07:00PM
```

**5. Last-minute cancellation (PartyOn's key feature).** Giovanna can't come anymore. Her RSVP of `No` releases everything she was bringing, and everyone sees at once what still needs covering, so Andrew grabs the punch.

```
Giovanna>  RSVP GradParty No
           200 OK
           [Notification] 210 GradParty ITEM_RELEASED Cake is available to bring again
           [Notification] 210 GradParty ITEM_RELEASED Punch is available to bring again
Andrew     [Notification] 210 GradParty RSVP_UPDATED Giovanna status is now No
           [Notification] 210 GradParty ITEM_RELEASED Cake is available to bring again
           [Notification] 210 GradParty ITEM_RELEASED Punch is available to bring again
Andrew>    CLAIM_ITEM GradParty Punch
           200 OK
           [Notification] 210 GradParty ITEM_CLAIMED Punch claimed by Andrew
Andrew>    WHO GradParty
           200 OK
           Andrew (organizer) - Maybe
           Giovanna - No
           LaNaysha - Maybe
           END
```

**6. LaNaysha's laptop dies (no `QUIT`).** The others keep working, and her claims stay on the list. Giovanna still wants to drop off balloons, but `No` means "not coming and not bringing anything", so she first changes her RSVP to `Maybe`.

```
Giovanna>  CLAIM_ITEM GradParty Balloons
           410 Conflict: you RSVP'd No; change your RSVP to Yes or Maybe to claim items
Giovanna>  RSVP GradParty Maybe
           200 OK
Giovanna>  CLAIM_ITEM GradParty Balloons
           200 OK
           [Notification] 210 GradParty ITEM_CLAIMED Balloons claimed by Giovanna
Andrew     [Notification] 210 GradParty RSVP_UPDATED Giovanna status is now Maybe
           [Notification] 210 GradParty ITEM_CLAIMED Balloons claimed by Giovanna
Andrew>    ITEMLIST GradParty
           200 OK
           Cake - unclaimed
           Chips - claimed by LaNaysha
           Punch - claimed by Andrew
           Balloons - claimed by Giovanna
           END
Andrew>    QUIT
           Goodbye!
```

If the server is restarted now, it logs `Loaded 1 event`, and anyone who `JOIN`s `GradParty` again finds the same guests, RSVPs, and items.

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

**Every error line says what went wrong.** Errors are still a single line that starts with the code and phrase, followed by `: ` and a plain-language reason:

```
300 Message Format Error: invalid date in field 3 (expected a real date as MM/DD/YYYY)
300 Message Format Error: wrong number of fields (got 1), usage: CLAIM_ITEM <EventCode> <ItemName>
401 Not Identified: this connection has not joined that event
410 Conflict: item already claimed by Andrew
```

Programs should look only at the 3-digit code. The reason is for people and may change.

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
 accept loop (main thread)
      │  one session per connection
      ▼
 reader thread ──► LineFramer ──► Dispatcher ──► AppState ──────────► EventStore (data/*.event)
 (per client)                     (validate,     (lock the event,     save before replying
                                   300 errors)    apply the command)
                                                        │
                                                        │ response + notifications,
                                                        │ queued while the lock is held
                                                        ▼
                                      per-session output queues ──► writer thread (per client) ──► socket
```

- Each connection has a **reader thread** (framing, parsing, applying commands) and a **writer thread** (sending that session's queued output).
- A command for an event runs entirely under **that event's lock**: commands for one event are applied one at a time (first come, first served), and commands for different events run in parallel.
- The change is **saved to disk** and the response and notifications are **queued while the lock is held**. Every connection therefore sees an event's changes in the order they were applied, a response is never interrupted by a notification, and a client always gets the response to its own command before any notification that command caused.
- Sockets are written only by writer threads, never while holding a lock, so a slow client can't delay anyone else.

The comments at the top of `server/AppState.hpp` and `server/Session.hpp` describe the locking rules in detail.

### Data model

- `events: map<EventCode, EventEntry>`, guarded by one mutex for lookups
- `EventEntry { mutex, Event, deleted, connections: map<SessionId, UserName> }`: the sessions currently identified in the event, so notification recipients are read under the event's lock
- `Event { eventCode, organizerName, date, time, guests[], items[] }`
- `Guest { userName, rsvpStatus (Yes|No|Maybe), isOrganizer }`
- `Item { itemName, claimedBy (empty = unclaimed) }`
- `Session { id, socket, output queue, writer thread }`, kept in a `SessionRegistry`

---

## 10. Design decisions

| Topic | Decision | Notes |
|---|---|---|
| Default port | **5527** (was 5000 in early testing) | Port 5000 is taken by AirPlay Receiver on macOS. 5527 nods to CIS 527 and the 527-byte line limit. |
| Command on a deleted event | **`401 Not Identified`** | Changed from the original data design, which said `404`. Only `JOIN` returns `404`. |
| Empty line | **`300 Message Format Error`** | Changed from the original test plan (TC-F07, "ignored"). |
| Same username joins again | **Rejoin**, keeping their RSVP status | Replaces requirement FR-20 ("reject duplicate usernames"). There is no authentication. |
| Error responses | **`<code> <phrase>: <reason>`**, e.g. `300 Message Format Error: invalid date in field 3 (…)` | Instructor feedback: users couldn't tell what to fix when the reason was only in the server log. Every error now includes its reason on the same line. |
| JOIN reply shows date/time | Second line: `<User>, you are now a guest of <Event> (<Date> <Time>).` | Covers requirement FR-05 (view event date and time). |
| Client oversized lines | The client also enforces the 527-byte receive limit | Required by the spec's buffering rules for both sides. |
| Cancelling releases items | `RSVP No` releases all of the guest's claimed items in the same step, and blocks new claims until the RSVP changes to `Yes` or `Maybe`. The organizer follows the same rule but can still update and delete the event. `RELEASE_ITEM` is always allowed | PartyOn's distinctive feature from the original project proposal (§2g): when someone cancels, what they were bringing becomes available again, and everyone is told what still needs covering. |
| Concurrency | Thread per connection (reader + writer), one lock per event | First come, first served per event; different events in parallel; 1000-line output limit per client. |
| Saving to disk | One text file per event in `--data-dir`, written to a temp file and renamed | A crash never leaves a half-written event; file names are the hex event code because codes are case-sensitive and may contain `/`. |

---

## 11. Testing

```bash
make test                             # unit tests: framing, state, concurrency, saving (no network)
make tsan                             # the same tests under ThreadSanitizer (data-race detector)

./bin/partyon-server &                # then, against the running server:
python3 tests/protocol_test.py        # protocol conformance          (optional: [host] [port])
python3 tests/concurrency_test.py     # multi-client races and failures (optional: [host] [port])
```

| Suite | What it checks |
|---|---|
| `test_framer`, `test_state` | Framing (TC-F01–F09), every command and response code, error reasons |
| `test_concurrency` | 16 threads racing for one item, same-code creates, delete-vs-claim, per-connection notification order, connect/disconnect churn |
| `test_store` | Save/load round trip, damaged or half-written files skipped, every change saved, a losing `CREATE_EVENT` never overwrites the winner |
| `protocol_test.py` | Every response code and framing case over real TCP |
| `concurrency_test.py` | The protocol spec's conflict table and the test plan's TC-C01–C06 with simultaneous clients, plus cancellation rules, failure isolation, and slow-client protection |
| `persistence_test.py` | Starts its own server: crash (kill) and restart, Ctrl+C with clients connected, damaged event file. Run with `python3 tests/persistence_test.py` |
| `load_test.py` | Throughput, latency, and broadcast time for 10–200 clients, in one shared event and in separate events. Run with `python3 tests/load_test.py --clients 10,50,100` |

The Python scripts print PASS or FAIL for each test ID, and they work against any running server, including one on another machine.

**Automatic checks (GitHub Actions):** every push builds the project and runs all of the above on **Linux and Windows**, plus a ThreadSanitizer job that checks the live server for data races (`.github/workflows/build.yml`). See the repo's **Actions** tab, or the ✅/❌ next to a commit or PR. To start a run by hand, open Actions → *Build and test* → **Run workflow**.

Manual and integration testing follows the test plan in `docs/Project - Phase 1.pdf` (§9). A sample server log from a three-client session is in `logs/sample-server.log`, and screenshots are in `docs/screenshots/`. The framing tests cover one command per write, byte-by-byte delivery, several commands in one write, a split mid-token, a UTF-8 character split across reads, `\r\n` endings, a connection closed mid-line, and oversized lines.

To send raw protocol lines without the client:

```bash
nc 127.0.0.1 5527
CREATE_EVENT Party1 Andrew 10/31/2026 07:00PM
```

---

## 12. Known limitations

- **One thread pair per connection.** Each client uses two threads (reader and writer). This is simple and portable, but memory and scheduling cost grow with the number of clients. Our load test ran 200 simultaneous clients with no errors; thousands would need an event loop (`epoll`/`kqueue`/IOCP) instead.
- **One busy event is the bottleneck.** Commands affecting the same event are processed one at a time, and item claims generate notifications for connected guests. In our Phase 3 load tests, shared-event throughput decreased from 365 commands per second with 10 clients to 117 commands per second with 200 clients. When clients worked on separate events, throughput remained between 1,048 and 1,244 commands per second. This shows that event-level synchronization helps maintain consistent data, but can also limit performance when many clients interact with the same event. See tests/Phase3-TestReport.md for the complete measurements.
- **Failed disk writes are not reported to the user.** If saving an event fails (for example, the disk is full), the server logs an error and keeps the change in memory, but still replies with success. The protocol has no response code for "saved in memory only".
- **No authentication.** Anyone who knows an event code can `JOIN` under any name, including the organizer's (spec: "a rejoin").
- **Sessions are not saved.** After a server restart, every user must `JOIN` again. Events, guests, RSVPs, and items are restored.
- **Plain TCP.** Traffic is not encrypted. Across the internet, use a VPN (we tested over the school VPN).

## 13. Attribution

PartyOn uses **no external libraries**: only the C++17 standard library, POSIX sockets / Winsock2 from the operating system, and the Python 3 standard library for the test scripts. The protocol, server, client, and tests were written by Group 4 for CIS 527. Some code and documentation were drafted with help from an AI assistant (Claude) and reviewed by the team.

## 14. Troubleshooting

| Problem | Fix |
|---|---|
| `bind failed: Address already in use` | Another program, or an earlier server run, is using the port. Wait a few seconds, or run on another port (`./bin/partyon-server 6000`). |
| Client prints `Could not connect to the PartyOn server.` | Check that the server is running, that the host and port are right, and that the firewall allows the port. |
| Two machines: connection times out, nothing appears in the server log | Make sure **both** machines are connected to the VPN (the client must have a `141.215.x.x` address too), and add the macOS firewall rule from §6. |
| `socketfilterfw`: "The file path you specified does not exist" | Run the command from the repository root, or give the full path to `bin/partyon-server`. |
| `ifconfig \| grep utun` shows no address while on the VPN | Use the `awk` command in §6. It lists every interface with an IPv4 address. |
| Client connects but nothing happens | Check the server terminal for a `session N connected` line. If it's missing, the client reached a different machine or port. |
| Every command returns `300` | Commands are case-sensitive and uppercase, with single spaces and no extra fields. |
| `401 Not Identified` | `JOIN` or `CREATE_EVENT` the event on this connection first. Identities are lost when you reconnect. |

---

## 15. Future work

Ideas from the original proposal and from our testing that are outside this project's scope:

- **Important items:** let the organizer mark items as essential (e.g. the cake) and alert them when one is released (requirements FR-14, FR-15).
- **Backup options and event history** (FR-16, FR-17).
- **Accounts and encryption:** authentication so names can't be impersonated, and TLS for traffic over the internet.
- **Scaling further:** an event loop (`epoll`/`kqueue`/IOCP) instead of two threads per client, for thousands of connections.
- **Reporting failed saves** to the user instead of only logging them.
