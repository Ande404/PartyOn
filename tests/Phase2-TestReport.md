# PartyOn – Phase 2 Test Report

> **Draft.** Server, framing, and integration results were filled in from test runs on 2026-10-12. LaNaysha to review, complete the open items in §7, and finalize the requirements traceability.

## 1. Scope

Phase 2 delivers a **Level 1** (one client at a time) PartyOn server and command-line client that follow `docs/UpdatedProtocolSpecification.md`. This report covers:

- message framing over TCP (Phase 1 test plan §9.3, TC-F01 to TC-F09)
- response-code coverage (§9.4)
- command behavior and team design decisions
- end-to-end runs with the real client
- logging and clean disconnects

Concurrency, race, and resilience tests (§9.5–9.6) need multiple simultaneous clients and are deferred to Phase 3.

## 2. Environment

| Item | Value |
|---|---|
| OS | macOS 14 (Darwin 23.1), Apple clang 15, C++17 |
| Network | loopback (`127.0.0.1`), port 5527 |
| Server build | `make server` (no warnings with `-Wall -Wextra`) |
| Client build | `make client` |
| Raw-socket tests | Python 3 (`tests/protocol_test.py`) |
| CI (GitHub Actions) | `ubuntu-latest` (g++) and `windows-latest` (MSYS2 UCRT64, MinGW-w64 g++), run on every push |

## 3. How to reproduce

```bash
make                     # builds bin/partyon-server and bin/partyon-client
make test                # unit tests (no network)

./bin/partyon-server &   # in another terminal
python3 tests/protocol_test.py            # conformance tests over TCP
```

## 4. Results summary

| Suite | What it covers | Result |
|---|---|---|
| `tests/test_framer.cpp` | Line framing, TC-F01–F09, 527-byte boundary | **60 / 60 checks pass** |
| `tests/test_state.cpp` | All commands, all response codes, error precedence, notification recipients | **93 / 93 checks pass** |
| `tests/protocol_test.py` | Live server over TCP: framing, response codes, behavior | **23 / 23 tests pass** |
| End-to-end with `partyon-client` | Real client against real server, scripted session | **Pass** (display notes in §6) |
| Client oversized-line handling | Fake server sends 600- and 700-byte lines | **Pass**: warning shown, next reply displayed |
| **Two machines over VPN** (macOS server, Windows client) | Interactive client session + full conformance suite across the network | **Pass**: 23 / 23, see §5.4 |
| GitHub Actions: Linux and Windows | Build (no warnings), unit tests, conformance tests on a live server | **Pass on both** ([run 38276372268](https://github.com/gponce1/PartyOn-Phase2/actions/runs/38276372268)) |

## 5. Detailed results

### 5.1 Framing (Phase 1 §9.3)

| ID | Objective | Expected | Result |
|---|---|---|---|
| TC-F01 | One command, one write | Parsed and executed once | Pass |
| TC-F02 | One command sent byte by byte | Parsed once, no timeouts | Pass |
| TC-F03 | Three commands in one write | Three responses, in order | Pass |
| TC-F04 | Command split mid-token | Reassembled correctly | Pass |
| TC-F05 | Multi-byte UTF-8 split across two writes | Reassembled correctly (`José`) | Pass |
| TC-F06 | `\r\n` line endings | Accepted, `\r` stripped | Pass |
| TC-F07 | Empty line | **300** (team decision; Phase 1 plan said "ignored") | Pass |
| TC-F08 | Connection closed mid-line | Partial command not executed | Pass |
| TC-F09 | Line over 527 bytes | 300, rest of line discarded, next line works | Pass |

The unit tests also check the exact limit: a 527-byte line including `\n` is accepted and a 528-byte line is rejected.

### 5.2 Response-code coverage (Phase 1 §9.4)

| Code | Trigger tested | Test ID | Result |
|---|---|---|---|
| 200 OK | Valid RSVP, CLAIM, RELEASE, UPDATE, WHO, ITEMLIST | RC-200, RC-210 | Pass |
| 201 Created Event | Valid CREATE_EVENT | RC-201 | Pass |
| 202 Deleted Event | Organizer DELETE_EVENT | RC-202 | Pass |
| 210 Notification | ITEM_CLAIMED, ITEM_RELEASED, EVENT_UPDATED, EVENT_DELETED, RSVP_UPDATED, each arriving after its response | RC-202, RC-210, RC-200 | Pass |
| 300 Message Format Error | Bad RSVP status, unknown/lowercase command, double space, extra field, `HELP` sent to server, invalid date/time, 101-byte field, invalid UTF-8, oversized line | RC-300, TC-F07, TC-F09 | Pass |
| 401 Not Identified | RSVP before JOIN; command on a deleted event | RC-401, TC-B03 | Pass |
| 402 Not Authorized | Guest UPDATE_EVENT / DELETE_EVENT | RC-402 | Pass |
| 404 Not Found | JOIN to non-existent or deleted event | RC-404, TC-B03 | Pass |
| 410 Conflict | Duplicate event code, claim of a claimed item, release of an unknown item, double JOIN | RC-410 | Pass |

Error precedence checks: a malformed command returns 300 even when the user isn't identified (RC-401) and even when the user isn't the organizer (unit tests).

### 5.3 Behavior and design decisions

| ID | Behavior | Result |
|---|---|---|
| TC-B01 | JOIN reply shows the event date and time (FR-05) | Pass |
| TC-B02 | Rejoining with the same name keeps the guest's RSVP | Pass |
| TC-B03 | Deleted event: 401 for commands, 404 for JOIN, code reusable, old identities not carried over | Pass |
| TC-B04 | QUIT: no response, server closes the socket and accepts the next client | Pass |
| TC-B05 | One connection identified in two events under different names | Pass |
| — | Abrupt client disconnect: session removed, server keeps running | Pass (raw-socket run) |
| — | Ctrl+C on the server: sockets closed, exit code 0 | Pass (macOS) |

### 5.4 Two-machine test (Phase 1 §9.2)

| | Server | Client |
|---|---|---|
| OS / build | macOS, Apple clang | Windows, MinGW-w64 GCC 15.2 (`g++ -std=c++17 client/client.cpp -lws2_32 -pthread`) |
| Network | UM-Dearborn GlobalProtect VPN, `141.215.94.35` | Same VPN from a home network, `141.215.95.94` |

| Step | Result |
|---|---|
| Reachability: `Test-NetConnection 141.215.94.35 -Port 5527` from Windows | `TcpTestSucceeded : True` |
| Interactive `partyon-client.exe` session: CREATE_EVENT, CLAIM_ITEM, WHO, ITEMLIST, RSVP, QUIT | All responses and notifications correct; clean disconnect |
| `python tests/protocol_test.py 141.215.94.35 5527` from Windows | **23 / 23 pass** |

The full server log for this run is in `docs/two-machine-log.txt`. Setup notes:
- The client machine has to be connected to the VPN. A first attempt from the home network failed, as expected, because the VPN address can only be reached from inside the VPN.
- macOS's firewall blocks our unsigned server binary until an allow rule is added (`socketfilterfw --add` / `--unblockapp`). The README troubleshooting section has the commands.

### 5.5 Logging

Every connection, received command, response code (with the reason for errors), notification, and disconnect is logged with a timestamp to the terminal and `server.log`. A sample from three client sessions is in `docs/server-log-sample.txt`.

## 6. Issues found

| # | Area | Issue | Status |
|---|---|---|---|
| 1 | Client | Did not discard incoming lines over 527 bytes (spec requires it of both sides) | **Fixed** on `aluwaga-feature/integration` |
| 2 | Mock server | Used port 5000 and `300 Invalid Command` | **Fixed** (5527, `300 Message Format Error`) |
| 3 | Mock server | Did not compile on macOS: `bind` resolved to `std::bind` | **Fixed** (`::bind`) |
| 4 | Client display | A notification that arrives just after its response prints a second `PartyOn>` prompt | Open (cosmetic), client owner |
| 5 | Client display | A blank line is printed before every server line, so multi-line replies show blank lines between rows | Open (cosmetic), client owner |
| 6 | Client help | `HELP` lists command names only, without their arguments (NFR-02 / NFR-07) | Open, client owner |

## 7. Open items

- [x] Build and test on **Windows**: build, unit tests, and conformance tests pass on GitHub Actions `windows-latest` (MSYS2 + MinGW-w64)
- [x] Interactive client session on a real Windows machine (§5.4)
- [x] One run **across two machines** (Phase 1 §9.2): macOS server, Windows client, over the school VPN (§5.4)
- [ ] Optional: run the *server* on Windows and check that Ctrl+C shuts it down cleanly (CI only kills the process)
- [ ] Final requirements traceability review (draft below)

## 8. Requirements traceability (draft)

| Req | Summary | Phase 2 status | Evidence |
|---|---|---|---|
| FR-01 | Connect by host/IP and port | Met | Client prompts; E2E run; two-machine run over VPN (§5.4) |
| FR-02 | Provide a username | Met | JOIN / CREATE_EVENT |
| FR-03 | Join an existing event | Met | TC-B01, RC-404 |
| FR-04 | Confirm or change attendance | Met | RC-200, TC-B02 |
| FR-05 | View event code, date, and time | Met | TC-B01 (JOIN reply), CREATE_EVENT, EVENT_UPDATED |
| FR-06 | View the contribution list | Met | ITEMLIST |
| FR-07 | Submit or claim an item | Met | RC-210 |
| FR-08 | See who brings each item | Met | ITEMLIST |
| FR-09 | Server keeps shared lists and RSVPs | Met | Unit tests, TC-B02 |
| FR-10 | Multiple users on the same event | Partial: users take turns (Level 1) | Phase 3 |
| FR-11 | Reject an item that is already claimed | Met (410); concurrent case in Phase 3 | RC-410 |
| FR-12 | Invalid request answered, no crash | Met | RC-300, TC-F09 |
| FR-13 | Exit option closes the connection | Met | TC-B04, E2E |
| FR-14, FR-15 | Important items and alerts | Future work | — |
| FR-16, FR-17 | Backup option, event history | Not implemented (Could Have) | — |
| FR-18 | Persist events across restarts | Not implemented | Phase 3 candidate |
| FR-19 | Several events per user | Met | TC-B05 |
| FR-20 | Reject duplicate usernames | **Superseded:** same name is a rejoin (team decision) | TC-B02 |
| FR-21 | Organizer can create an event | Met | RC-201 |
| FR-22 | Push updates to connected clients | Met for the connected client (Level 1); all clients in Phase 3 | RC-210 |
