# PartyOn - Phase 3 Multi-Client Test Report

**Course:** CIS 527 - Computer Networks  
**Project:** Socket Programming Project - Phase 3  
**Team:** Giovanna Ponce Cisternas, LaNaysha Simms, Andrew Luwaga  
**Test Date:** November 5, 2026   
**Version Tested:** Phase 3 / PartyOn Protocol v2.0

## 1. Scope

This report verifies the multi-client server implementation introduced in Phase 3. It
extends the earlier [Phase 2 test report](./Phase2-TestReport.md), which covered the
single-client protocol implementation.

The Phase 3 verification covers:

- concurrent command processing and first-come-first-served conflicts
- notification fan-out and ordering
- failure isolation between client sessions
- slow-client protection
- event persistence across crashes and restarts
- graceful shutdown with connected clients
- C++ unit tests for framing, state, concurrency, and persistence
- throughput, latency, and broadcast performance at 10, 50, 100, and 200 clients

## 2. Test Environment

| Item | Value |
|---|---|
| Operating system | Windows |
| Compiler | MinGW-w64 `g++` |
| C++ standard | C++17 |
| Python | Python 3.14.8 virtual environment |
| Network | Local loopback (`127.0.0.1`) |
| Protocol port | `5527` for the live concurrency and load tests; persistence tests choose a free port |
| Dependencies | No third-party project dependencies |

The C++ test suite was built and run through the repository's official
`mingw32-make` workflow in the MSYS2 UCRT64 terminal. The live Python tests were run
using the project's configured Windows virtual-environment interpreter.

## 3. Reproduction Commands

From the repository root:

```powershell
# Build and run the four C++ test executables through the official Makefile
mingw32-make test

# Start the server for the live concurrency and load tests
./bin/partyon-server.exe 5527

# In another terminal:
python tests/concurrency_test.py 127.0.0.1 5527
python tests/load_test.py 127.0.0.1 5527 --clients 10,50,100,200

# Stop the manually started server, then run persistence tests separately.
# This test starts and stops its own server on temporary ports and data.
python tests/persistence_test.py ./bin/partyon-server.exe
```

If the command `python` is not available, use the Python command provided by the
local environment, such as `python3`, `py -3`, or the interpreter from a project's
virtual environment. The live tests were run against the Makefile-built server using
temporary data directories so that existing local files were not used as test state.

## 4. Results Summary

| Suite | Coverage | Result |
|---|---|---|
| `test_framer` | TCP line buffering, UTF-8 boundaries, CRLF, oversized lines | **60 / 60 checks passed** |
| `test_state` | Commands, response codes, error precedence, notifications | **145 / 145 checks passed** |
| `test_concurrency` | In-process concurrent state operations | **312 / 312 checks passed** |
| `test_store` | Save/load, damaged files, atomic persistence behavior | **45 / 45 checks passed** |
| `concurrency_test.py` | Live multi-client races, fan-out, failures, slow clients | **24 / 24 tests passed** |
| `persistence_test.py` | Crash/restart, shutdown, identity reset, damaged file | **4 / 4 tests passed** |
| `load_test.py` | Throughput, latency, broadcast, 10-200 clients | **Completed with 0 errors and 0 dropped clients** |

**Overall result:** All executable Phase 3 checks run for this verification passed.
The load test completed successfully at every tested client count. The Windows
crash-style persistence case is reported by the test as skipped because Windows
does not provide the POSIX `SIGINT` behavior used by that case; the remaining
persistence checks passed.

## 5. Detailed Multi-Client Results

### 5.1 C++ Unit and Concurrency Tests

The C++ tests run without a network server and verify the internal behavior of
framing, command/state handling, concurrency, notification delivery, and
persistence.

#### `test_framer`

| ID | Test objective | Expected result | Actual result | Status |
|---|---|---|---|---|
| TC-F01 | One complete command in one write | One complete line is returned with no pending bytes | `WHO Party1` was returned once; buffer was empty | **Pass** |
| TC-F02 | Command delivered one byte at a time | No line before the final byte; one complete line afterward | `JOIN Party1 User1` was reconstructed once | **Pass** |
| TC-F03 | Three commands in one write | Three lines are returned separately and in order | `WHO`, `ITEMLIST`, and `RSVP` were separated correctly | **Pass** |
| TC-F04 | Command split in the middle of a token | Partial input is buffered and reconstructed | `CLAIM_ITEM Party1 Chips` was reconstructed; partial `WHO` remained buffered | **Pass** |
| TC-F05 | Multi-byte UTF-8 character split across reads | UTF-8 bytes are reassembled without corruption | `José` was reconstructed and validated as UTF-8 | **Pass** |
| TC-F06 | CRLF line endings | `\r` is removed and both lines are returned | Both commands were returned without the carriage return | **Pass** |
| TC-F07 | Empty lines | Empty lines are passed to command handling | Two empty lines were returned for `\n\r\n` | **Pass** |
| TC-F08 | Connection closes mid-line | Incomplete input is not executed | Partial command remained buffered and produced no complete line | **Pass** |
| TC-F09 | Maximum and oversized lines | Exact limit is accepted; oversized input is reported once, discarded through newline, and followed by valid input | Boundary and recovery behavior passed, including oversized input split across reads | **Pass** |

Result: **60 / 60 checks passed**.

#### `test_state`

| Test function | Test objective | Expected result | Actual result | Status |
|---|---|---|---|---|
| `formatErrors` | Validate malformed commands, fields, UTF-8, dates, times, RSVP values, and line length | Invalid input returns `300 Message Format Error` with a useful reason | All format errors and exact reason strings matched | **Pass** |
| `createAndJoin` | Create events, reject duplicate codes, join existing events, and list participants | Correct `201`, `200`, `404`, and `410` responses; valid WHO output | Creation, joining, duplicate handling, and WHO output matched | **Pass** |
| `rejoinKeepsRsvp` | Close and reopen a connection, then rejoin as an existing guest | Guest record and RSVP status survive; connection identity does not | RSVP was preserved and rejoin restored access | **Pass** |
| `rsvp` | Change RSVP status and validate RSVP-related rules | Valid RSVP changes succeed; invalid identity/status cases return correct codes | RSVP behavior and notifications matched | **Pass** |
| `items` | List, claim, and release items | Item state changes correctly; conflicts and notifications use the expected responses | Item operations and conflict rules matched | **Pass** |
| `updateEvent` | Update event date/time and enforce organizer authorization | Organizer update succeeds and notifies identified connections; guests receive `402` | Update, authorization, and notification behavior matched | **Pass** |
| `deleteEvent` | Delete events, notify connections, invalidate identities, and reuse codes | Delete returns `202`; later access requires rejoin; code can be reused | Deletion, identity removal, notification, and reuse behavior matched | **Pass** |
| `multipleEventsPerConnection` | Allow one connection to belong to multiple events | Commands and identities remain independent per event | Both events were usable and both identities were removed on close | **Pass** |
| `rsvpNoReleasesItems` | Release all items owned by a guest who RSVPs `No` | Only that guest's items are released and all affected connections are notified | Items, RSVP state, and ordered notifications matched | **Pass** |
| `rsvpNoBlocksClaims` | Prevent claims while RSVP is `No` and restore claim ability for `Maybe`/`Yes` | Claims return `410` while `No`; organizer management remains available | Restrictions and transitions matched | **Pass** |
| `stateErrorReasons` | Verify state-level error response wording and reasons | Errors contain the correct protocol code and explanatory reason | Error lines matched expected output | **Pass** |
| `quit` | Handle QUIT as a connection-close result | No response is generated and the result requests connection closure | QUIT behavior matched | **Pass** |

Result: **145 / 145 checks passed**.

#### `test_concurrency`

| Test function | Test objective | Expected result | Actual result | Status |
|---|---|---|---|---|
| `sameItemRace` | Sixteen clients race to claim each of 25 items | Exactly one `200` winner and 15 `410` conflicts per item; every item has one claimer | All 25 races produced one winner and consistent final state | **Pass** |
| `sameCodeCreateRace` | Sixteen clients create the same event code simultaneously | Exactly one event is created; 15 requests receive conflict; one guest remains | One event was created and all losing requests conflicted | **Pass** |
| `differentEventsInParallel` | Eight events process 200 claims each concurrently | Operations on independent events all succeed | All 1,600 claims succeeded across the eight events | **Pass** |
| `notificationOrder` | Eight clients submit 50 claims each while notifications are recorded | Every connection receives the same ordered notifications; requester response precedes its notification | Notification sequences matched across clients and response-before-notification ordering held | **Pass** |
| `deleteRacesWithClaims` | Delete an event while eight guests race to claim items, repeated for 20 rounds | Each claim is either applied before deletion or rejected with `401`; deleted event has no remaining snapshot | All 20 delete races produced valid serialized outcomes | **Pass** |
| `sessionChurn` | Six churn threads repeatedly join, RSVP, query, and close sessions while another client claims/releases | No bad session cleanup; guest records persist; remaining organizer can still operate | 1,200 churn iterations completed with valid cleanup and continued operation | **Pass** |

Result: **312 / 312 checks passed**.

#### `test_store`

| Test function | Test objective | Expected result | Actual result | Status |
|---|---|---|---|---|
| `serializeRoundTrip` | Serialize and parse an event, including UTF-8, guests, RSVP values, and items | Valid event round-trips exactly; truncated, malformed, invalid-date, and invalid-RSVP files are rejected | Round-trip and rejection checks matched | **Pass** |
| `fileNames` | Generate safe event filenames | Case-sensitive codes remain distinct and event-code characters cannot create path separators | Filename encoding checks passed | **Pass** |
| `saveLoadDelete` | Save, overwrite, load multiple events, and delete one | Latest event state loads correctly; deletion removes only the requested event | Save/load/overwrite/delete behavior matched | **Pass** |
| `damagedFilesAreSkipped` | Load a directory containing valid, incomplete, temporary, and unrelated files | Valid events load; damaged files generate warnings; temporary files are removed; unrelated files are ignored | One valid event loaded, two warnings reported, and temporary file removed | **Pass** |
| `appStateSavesChanges` | Verify every state change is persisted before response and restored after restart | Event date/time, guests, RSVPs, claims, releases, and deletion survive; identities do not | Restored state matched all expected values and required rejoin | **Pass** |
| `cancellationIsSaved` | Persist RSVP `No` together with all item releases | RSVP status and released items survive restart; claim restriction remains active | Cancellation state and restrictions were restored correctly | **Pass** |
| `deleteThenRecreate` | Delete and recreate an event code while testing a losing duplicate create | New event's file remains authoritative; losing create cannot overwrite it | New organizer/date remained intact and only one event loaded | **Pass** |

Result: **45 / 45 checks passed**.

### 5.2 Live Multi-Client Concurrency Tests

`tests/concurrency_test.py` ran against a live server on `127.0.0.1:5527`.

| ID | Test objective | Expected result | Actual result | Status |
|---|---|---|---|---|
| TC-C01 | Two clients claim the same item | One `200`, one `410` | One winner and one conflict | **Pass** |
| TC-C02 | Three and five clients claim the same item | Exactly one winner in each race | Exactly one winner in each race | **Pass** |
| TC-C03 | All connections receive the winning claim notification | Every identified connection receives the same notification | All connections notified | **Pass** |
| CT-2 | Clients claim different items in one event concurrently | Both claims succeed | Both returned `200` | **Pass** |
| TC-C06 | Clients modify different events concurrently | Both operations succeed | Both succeeded | **Pass** |
| TC-C04 | Clients create the same event code concurrently | One `201`, one `410` | One created, one conflict | **Pass** |
| TC-C05 | Release races another client's claim | One consistent first-come result | Valid release/claim outcome observed | **Pass** |
| CT-6 | Non-claimer releases a claimed item | `410 Conflict` | `410 Conflict` | **Pass** |
| CT-7 | Delete races a guest's claim | One consistent delete/claim outcome | Valid serialized outcome observed | **Pass** |
| CT-8 | Two organizer connections delete one event | One `202`, one `401` | `202` and `401` returned | **Pass** |
| CT-9 | Delete and recreate an event code | Old guest must rejoin before access | Old identity received `401` until JOIN | **Pass** |
| CT-10 | Update races a new guest JOIN | Valid ordering and notification behavior | Valid serialized outcome observed | **Pass** |
| CT-11 | Two connections JOIN with the same username | Both may join and receive notifications | Both returned `200` and were notified | **Pass** |
| CT-12 | One connection sends JOIN twice | First JOIN succeeds; second returns `410` | `200` then `410` | **Pass** |
| CT-13 | Client disconnects after a command | Change remains and other clients are notified | Change remained and notification arrived | **Pass** |
| CT-14 | Client stops reading responses | Slow client is dropped; others continue | Slow client dropped; other clients unaffected | **Pass** |
| CT-15 | Two guests RSVP concurrently | Organizer receives both updates | Both notifications received | **Pass** |
| TC-V01 | RSVP `No` releases claimed items | Items released and all connections notified | Releases and notifications correct | **Pass** |
| TC-V02 | RSVP `No` races another claim | One consistent state | Consistent outcome observed | **Pass** |
| TC-V03 | Guest cannot claim after RSVP `No` | Claim blocked until `Maybe` or `Yes`; organizer can manage | Expected restrictions enforced | **Pass** |
| TC-V04 | Guest claim races their RSVP `No` | Guest ends with no claimed items | Guest held no items | **Pass** |
| TC-I01 | Malformed input and abrupt disconnect | Other clients remain unaffected | Other clients remained unaffected | **Pass** |
| TC-I02 | Client remains stuck mid-line | Other clients continue processing | Other clients continued processing | **Pass** |
| TC-I03 | Ten clients connect simultaneously | All ten clients are served | All ten clients served | **Pass** |

Result: **24 / 24 tests passed**.

The slow-client test took approximately 66.56 seconds because it intentionally waits
for the server's 1,000-line output queue protection to disconnect a client that
stops reading.

### 5.3 Persistence and Shutdown Tests

`tests/persistence_test.py` starts its own server instances with temporary ports
and data directories:

| ID | Test objective | Expected result | Actual result | Status |
|---|---|---|---|---|
| PR-01 | Kill and restart the server | State matches the state before the kill | Passed; the POSIX signal-specific portion was skipped on Windows | **Pass*** |
| PR-02 | Ctrl+C with connected clients | Clients close cleanly and state is restored | Clients saw a close, exited cleanly, and state was restored | **Pass** |
| PR-03 | Restart without saving sessions | Users must JOIN again after restart | Identities were not restored; JOIN was required | **Pass** |
| PR-04 | Start with a damaged event file | Damaged file is skipped and valid state remains available | Server started and served the remaining valid data | **Pass** |

Result: **4 / 4 test cases passed**, with the crash-style case explicitly reporting
that its POSIX signal-specific assertion is skipped on Windows.

`*` PR-01 is marked as passed by the test suite, with the Windows-specific skip
noted in the output.

## 6. Scalability and Load Results

`tests/load_test.py` was run with 20 claims per client in two scenarios:

- **Shared:** every client uses one event, creating per-event lock contention and
  notification fan-out.
- **Separate:** each client uses its own event, allowing independent event locks to
  process work in parallel.

The load script does not assign test IDs, so this report assigns IDs to each
scenario/client-count run for traceability.

| ID | Test objective | Expected result | Actual result | Status |
|---|---|---|---|---|
| LOAD-S10 | Shared event with 10 clients | Complete 200 commands without errors or drops | 365 commands/sec; 0 errors; 0 drops | **Pass** |
| LOAD-D10 | Separate events with 10 clients | Complete 200 commands without errors or drops | 1,244 commands/sec; 0 errors; 0 drops | **Pass** |
| LOAD-S50 | Shared event with 50 clients | Complete 1,000 commands without errors or drops | 305 commands/sec; 0 errors; 0 drops | **Pass** |
| LOAD-D50 | Separate events with 50 clients | Complete 1,000 commands without errors or drops | 1,208 commands/sec; 0 errors; 0 drops | **Pass** |
| LOAD-S100 | Shared event with 100 clients | Complete 2,000 commands without errors or drops | 196 commands/sec; 0 errors; 0 drops | **Pass** |
| LOAD-D100 | Separate events with 100 clients | Complete 2,000 commands without errors or drops | 1,048 commands/sec; 0 errors; 0 drops | **Pass** |
| LOAD-S200 | Shared event with 200 clients | Complete 4,000 commands without errors or drops | 117 commands/sec; 0 errors; 0 drops | **Pass** |
| LOAD-D200 | Separate events with 200 clients | Complete 4,000 commands without errors or drops | 1,116 commands/sec; 0 errors; 0 drops | **Pass** |

Result: **8 / 8 benchmark runs passed** with no errors or dropped clients.

| Scenario | Clients | Commands | Commands/sec | p50 ms | p95 ms | Max ms | Notifications | Broadcast ms | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Shared | 10 | 200 | 365 | 26.79 | 30.10 | 31.2 | 1,945 | 5.0 | 0 |
| Separate | 10 | 200 | 1,244 | 7.19 | 12.00 | 15.1 | 190 | - | 0 |
| Shared | 50 | 1,000 | 305 | 157.96 | 222.22 | 307.8 | 48,676 | 24.6 | 0 |
| Separate | 50 | 1,000 | 1,208 | 34.91 | 67.30 | 160.8 | 950 | - | 0 |
| Shared | 100 | 2,000 | 196 | 492.62 | 691.91 | 1,951.8 | 192,826 | 30.7 | 0 |
| Separate | 100 | 2,000 | 1,048 | 78.99 | 175.68 | 321.4 | 1,900 | - | 0 |
| Shared | 200 | 4,000 | 117 | 1,729.21 | 2,026.62 | 6,998.1 | 757,270 | 54.1 | 0 |
| Separate | 200 | 4,000 | 1,116 | 164.07 | 257.98 | 440.7 | 3,800 | - | 0 |

The test reached 200 simultaneous clients with no errors and no dropped clients.
Separate events maintained substantially higher throughput because commands did not
contend for one event lock. The shared-event case degraded as the client count grew
because commands for one event are serialized and each change generates notifications
to the connected guests.

The measured results also show that the shared-event bottleneck affects latency:
at 200 clients, p95 latency reached approximately 2.03 seconds and maximum observed
latency was approximately 7.00 seconds. Separate events at 200 clients achieved
1,116 commands per second with p95 latency of approximately 258 milliseconds.

These results describe the tested implementation and local Windows environment.
They are not a guarantee of capacity on different hardware, operating systems, or
network conditions.

## 7. Issues and Limitations

1. The crash-style persistence case is skipped on Windows by the test suite because
   its signal behavior is POSIX-specific.
2. One thread pair per connection is simple and effective for hundreds of clients,
   but an event-driven model such as IOCP, `epoll`, or `kqueue` would be a likely
   next step for much larger connection counts.
3. A single busy event is a throughput bottleneck by design because its state changes
   are serialized and notifications fan out to all identified clients.

## 8. Final Verification

- [x] C++ framing tests passed: 60 / 60
- [x] C++ state tests passed: 145 / 145
- [x] C++ concurrency tests passed: 312 / 312
- [x] C++ persistence/store tests passed: 45 / 45
- [x] Live multi-client tests passed: 24 / 24
- [x] Persistence and shutdown suite completed: 4 / 4 reported pass
- [x] Load test completed at 10, 50, 100, and 200 clients
- [x] No load-test errors or dropped clients
- [x] Official `mingw32-make test` workflow completed
- [x] Results recorded from the current `master` commit
