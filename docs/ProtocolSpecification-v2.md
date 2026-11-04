# PartyOn Protocol Specification

**Protocol version: PartyOn v2.0**

## Overview

The PartyOn protocol is a line oriented, text based client server protocol that will serve as the implementation guide for PartyOn. All commands and responses transmitted between client/server are UTF-8 text and terminated by a newline character (`\n`).

The server manages one or more events, in which each event has an organizer and any number of guests. Users identify themselves using an event code + display name when they `JOIN` or `CREATE_EVENT`. The server remembers this identity for the connection, so later commands only need to supply the event code.

The server is in charge of:

- Tracking active events, the creator of each event, guests attending, and the items people are bringing
- Tracking identification/session state for each connected client (a single connection may be identified in multiple events)
- Supporting multiple simultaneous clients, at least three (see Concurrency and Conflict Behavior)
- Pushing notifications to affected clients when relevant event data changes

The client allows the user to issue the following commands:

- `JOIN`
- `CREATE_EVENT`
- `DELETE_EVENT`
- `RSVP`
- `ITEMLIST`
- `CLAIM_ITEM`
- `RELEASE_ITEM`
- `UPDATE_EVENT`
- `WHO`
- `QUIT`

To allow a new user to understand all of the actions available to them, a `HELP` command is also available. It is client side only, to ensure usability/accessibility is maintained. After the execution of a command, the client returns to its command prompt, unless the command causes the client/server to terminate.

## Versioning

This document defines **PartyOn v2.0**. There is no version command and no handshake or negotiation: the version is not transmitted on the wire.

v2.0 replaces the two conformance levels of v1.0 (Level 1, single client; Level 2, multi client) with one requirement: every server serves multiple clients simultaneously, at least three. The commands, responses, and notification formats are identical to v1.0, so a v1.0 client works unchanged against a v2.0 server. Behavior that applies only when several clients are connected is defined in Concurrency and Conflict Behavior. A single connected client sees the same behavior as in v1.0.

## Connection and Initialization

- The server sends nothing when a connection is opened (no greeting). The client may send a command immediately.
- Immediately after connecting, the **client** displays the PartyOn banner and the command menu (the same output as `HELP`). This is purely client side and is not part of the wire protocol.
- A connection starts with no identities. Identity for an event is established by a successful `JOIN` or `CREATE_EVENT`. Any other command that refers to an event the connection is not identified in returns `401 Not Identified`.
- Command names are case sensitive and uppercase. An unknown or empty command receives a `300 Message Format Error`.

## Response Codes

| Code | Meaning |
| --- | --- |
| `200 OK` | The command was accepted/completed successfully. |
| `201 Created Event` | A new event was successfully created. |
| `202 Deleted Event` | An event has been successfully deleted. |
| `210 Notification` | Server is sending a notification to the client indicating that something in an event has changed. |
| `300 Message Format Error` | The command or a field within the command wasn't formatted correctly, is missing, is not valid UTF-8, or exceeded the maximum size. |
| `401 Not Identified` | The connection is not identified in the referenced event. This includes events that were never created, events that have been deleted, and events the connection never joined. |
| `402 Not Authorized` | The identified user has not created the event and therefore does not have authorization to execute the command. |
| `404 Not Found` | A `JOIN` referenced an event that does not exist. |
| `410 Conflict` | The action conflicts with the existing state (e.g. item already claimed, event code already taken, already identified in this event). |

### Error response format

Every error response (`300`, `401`, `402`, `404`, `410`) is a single line of the form `<code> <phrase>: <reason>`. For example:

```
300 Message Format Error: invalid date in field 3 (expected a real date as MM/DD/YYYY)
300 Message Format Error: invalid RSVP status in field 2 (expected Yes, No, or Maybe)
401 Not Identified: not identified in event Party1
402 Not Authorized: only the organizer can delete this event
404 Not Found: event Party1 does not exist
410 Conflict: item already claimed by User1
410 Conflict: you RSVP'd No; change your RSVP to Yes or Maybe to claim items
```

Clients must act only on the 3-digit code. The reason is for people to read and may change, so clients must not parse it or depend on its wording. The error lists under each command name errors by code and phrase only, and the server always adds a reason. The reasons shown in this document are examples.

### Error precedence

When a command could fail for more than one reason, the server checks in this order and returns the first match.

For `JOIN`:

1. `300` - the command or any field is malformed
2. `404` - the event does not exist
3. `410` - the connection is already identified in this event

For `CREATE_EVENT`:

1. `300` - the command or any field is malformed
2. `410` - the event code is already in use

For all other commands:

1. `300` - the command or any field is malformed
2. `401` - the connection is not identified in the event (this is also the response for an event that does not exist)
3. `402` - the user is identified but is not the organizer (organizer-only commands)
4. `410` - the action conflicts with current state

Because identities are removed from every session when an event is deleted, `404` is only ever returned by `JOIN`.

## Message Encoding and Limits

- All text is UTF-8. A message that is not valid UTF-8 is rejected with `300 Message Format Error`.
- Fields are separated by single ASCII spaces. `<EventCode>`, `<UserName>`, and `<ItemName>` must be non-empty, must not contain any ASCII whitespace (space, tab, `\r`, `\n`), and must be at most 100 bytes (UTF-8 encoded). Event codes, usernames, and item names are case sensitive.
- The maximum message size is **527 bytes**, including the terminating `\n`. This limit is measured in bytes, not characters. It applies to every line sent in either direction. A command over this limit is rejected with `300 Message Format Error`.
- With the 100-byte field caps, the longest line the server can generate (an `ITEM_CLAIMED` notification) is well under 527 bytes.
- The server must tolerate and strip a trailing `\r` before the `\n`. Servers and clients send plain `\n`.

## Receiver-Side Buffering

TCP is a byte stream and does not preserve message boundaries. Both the client and the server must follow these rules when reading:

- Accumulate received bytes in a per-connection buffer, and only process complete lines (ending in `\n`).
- A single read may contain several complete lines, or only part of a line. The receiver must process every complete line in the buffer and keep any trailing partial line until the rest arrives.
- If the buffer reaches more than 527 bytes without containing a `\n`, the receiver treats the line as oversized: the server replies `300 Message Format Error`, and the receiver discards all bytes up to and including the next `\n` (including bytes that arrive later) before processing resumes.
- When a connection closes with an incomplete line in the buffer, the partial line is discarded and not processed.

## Notification Format

Notifications notify the organizer and guests of changes made. All notifications follow the format:

```
210 <EventCode> <NotificationType> <DetailsOfNotification>
```

**Notification types:**

**`ITEM_RELEASED`** - A guest has released an item

```
Example: 210 Party1 ITEM_RELEASED Chips is available to bring again
```

**`ITEM_CLAIMED`** - A guest has claimed an item

```
Example: 210 Party1 ITEM_CLAIMED Chips claimed by User1
```

**`EVENT_UPDATED`** - Organizer changes an event detail

```
Example: 210 Party1 EVENT_UPDATED new event details: 10/15/2026 06:00PM
```

**`EVENT_DELETED`** - Organizer deletes the event

```
Example: 210 Party1 EVENT_DELETED This event has been deleted
```

**`RSVP_UPDATED`** - Guest changes their RSVP status

```
Example: 210 Party1 RSVP_UPDATED User1 status is now No
```

Notifications may arrive at any time. The client must be able to receive and display them asynchronously. Ordering rules are given under Message Atomicity and Ordering.

## Multi-line Responses

Some responses span more than one line. Every multi-line response has a way for the client to know where it ends:

| Command | Response shape |
| --- | --- |
| `JOIN`, `CREATE_EVENT` | Exactly two lines (the status line and a confirmation line). |
| `ITEMLIST`, `WHO` | A `200 OK` line, then zero or more body lines, then a line containing only `END`. |

The `END` line is always sent for `ITEMLIST` and `WHO`, even when the list is empty. Body lines always have the form `<name> - <status>`, so a bare `END` can never be confused with a body line, even if a username or item name is `END`. Error responses (`300`, `401`, `402`, `404`, `410`) are a single line with no `END` terminator.

## Message Atomicity and Ordering

- **A response is written as one uninterrupted block.** A complete response (single-line, two-line, or `200 OK` ... `END`) is never interleaved with a `210` notification. A notification is sent either before the response block starts or after it finishes, never in the middle.
- **Response before own-caused notification.** The requester always receives the response to their command before any notification caused by that same command (for example, `200 OK` precedes the `ITEM_CLAIMED` notification the claimer also receives as a guest).
- Responses are returned in the order the commands were received. A client may only assume that a line beginning with `210` is a notification when it is not inside a response block it is currently reading (that is, outside the lines between a `200 OK` for `ITEMLIST`/`WHO` and its `END`, and outside the second line of a `JOIN`/`CREATE_EVENT` response).

## Concurrency and Conflict Behavior

### Sessions

- Every accepted connection is a **session** with a unique session ID assigned by the server when the connection is accepted. The ID is used for logging only and is never sent on the wire.
- A session holds the connection's identities (event code to username). A session may be identified in several events, with a different username in each.
- A session ends on `QUIT`, an abrupt disconnect, a read or write error, or being dropped as a slow client (see Slow Clients). When a session ends, the server removes all of its identities, discards its pending output, and closes its socket. Guest records, RSVP statuses, and claimed items remain in their events.
- An error caused by one client (malformed input, a dropped connection, an oversized line) affects only that client's session. Other sessions continue normally.

### Simultaneous Clients

- The server must serve at least **three** clients simultaneously, with no fixed maximum.
- A client that is idle, slow, or in the middle of a partial line must not prevent the server from reading and answering other clients.
- Clients working in **different events** are processed concurrently and never wait on each other.

### First Come, First Served

- All commands that read or change a single event are applied **one at a time, in the order the server acquires that event's lock**. "First" means first to be applied by the server, not first to arrive on the network. Commands arriving at nearly the same instant are ordered by whichever the server applies first, and that order is final.
- Each command is evaluated against the event state at the moment it is applied, using the existing error precedence. The first command receives its normal success response. A later command that conflicts with the resulting state receives the standard error for that conflict (for example `410 Conflict` for claiming an already claimed item). No new response codes are introduced.
- A command is applied atomically: other commands for the same event never see a half-applied change. The event lock is held only while event data is read, changed, and persisted. It is never held while writing to a socket, so a slow client cannot delay other clients' commands.
- `CREATE_EVENT` is ordered across all events by the event code. If two clients create the same code, the first applied receives `201 Created Event` and the second receives `410 Conflict`.
- If an event is deleted while a command for it is waiting to be applied, the command is evaluated after the deletion: it receives `401 Not Identified` (or `404 Not Found` for `JOIN`). A later `CREATE_EVENT` that reuses the code makes a new event, and sessions identified in the old event are not identified in the new one. They must `JOIN` again.

### Notification Recipients and Ordering

- Recipients are the **connections identified in the event at the moment the command is applied**. Guests with no connection receive nothing. They see current state through `WHO` and `ITEMLIST` after they `JOIN`.
- A connection that joins after a change was applied does not receive that change's notification.
- The requester of `CLAIM_ITEM`, `RELEASE_ITEM`, and `UPDATE_EVENT` is a recipient and receives the notification after its response. The requester of `DELETE_EVENT` also receives the `EVENT_DELETED` notification after the `202`.
- `RSVP_UPDATED` is sent to the organizer's connections only, including when the organizer changes their own RSVP, so the behavior is the same for every role.
- Notifications for one event reach each connection in the order the changes were applied.
- The atomicity rules under Message Atomicity and Ordering still hold on every connection: a response block is never interleaved with a notification, and a command's response precedes any notification caused by that command.

### One Username on Multiple Connections

Because there is no authentication, two connections may identify as the same username in the same event (each `JOIN` is a rejoin). Each connection is independent and each receives notifications. The same connection joining the same event twice still receives `410 Conflict`. If two connections identified as the organizer's username send organizer-only commands, they are applied in lock order, with the last one applied taking effect.

### Disconnects

- If a client disconnects after its command was applied but before it received the response, the change stays applied and only the response is lost.
- If a client disconnects before its command is applied, the command is not applied. A partial line in the buffer is discarded as described under Receiver-Side Buffering.
- A disconnect never rolls back, deletes, or releases anything. Items a departed user claimed stay claimed until released by that user on a later connection.

### Slow Clients

The server queues output per session. If a client stops reading and the number of lines queued for it exceeds **1000 lines**, the server ends that session. Each client's socket send buffer is also capped at **64 KB**. This prevents one stalled client from delaying notifications to others. The limit is a count of queued lines, not a time delay.

### Persistence

Events are saved as **one file per event** in the folder given by the `--data-dir` option (default `data/`). Each change is saved as part of applying the command, before the response is sent. A save writes a temporary file and then renames it over the event's file, so a crash never leaves a half-written event. Writes for one event are serialized with that event's other commands. Connections and sessions are not saved.

### Logging

The server writes one log line per event, in the form `[<timestamp>] [<level>] <message>`. The timestamp is `YYYY-MM-DD HH:MM:SS`. The level is `INFO` for normal activity and `WARN` for a command that was rejected with an error. Log lines from concurrent sessions must not be interleaved within a line. Log wording is for operators and is not part of the wire protocol.

The server logs:

- **Startup:** how many events were loaded from the data folder, and the address, port, and per-client output limit it is listening with.
- **Session connect:** the session ID, the client's address, and how many sessions are now connected.
- **Each command applied:** `session <id> <COMMAND> <EventCode> -> <result>`, where the result is the response status line (with the reason for an error). Optional details follow: `(+N lines)` for extra response lines after the status line, `[new guest]` for a first-time `JOIN`, `[released N items]` for an `RSVP No` that released items, and `notified N connections` (with `(M notifications)` when more than one notification was sent per connection).
- **Rejected input:** a line that could not be parsed is logged as `session <id> rejected "<line>" -> <error>`.
- **`QUIT`:** logged with `no response`.
- **Session disconnect:** the reason (for example, the client closed the connection or sent `QUIT`), how many events the session was removed from, and how many sessions are still connected.
- **Shutdown:** the start of shutdown (with how many sessions are being closed) and when the server has stopped.

Example:

```
[2026-11-03 13:49:32] [INFO] session 2 CLAIM_ITEM GradParty -> 200 OK; notified 3 connections
[2026-11-03 13:49:32] [WARN] session 3 CLAIM_ITEM GradParty -> 410 Conflict: item already claimed by Giovanna
[2026-11-03 13:49:35] [WARN] session 3 rejected "RSVP GradParty Purple" -> 300 Message Format Error: invalid RSVP status in field 2 (expected Yes, No, or Maybe)
[2026-11-03 13:49:36] [INFO] session 2 RSVP GradParty -> 200 OK [released 2 items]; notified 3 connections (7 notifications)
[2026-11-03 13:49:39] [INFO] session 3 disconnected (client closed the connection); removed from 1 event; 2 session(s) still connected
```

### Graceful Shutdown

On a termination signal (SIGINT or SIGTERM) the server stops accepting connections, lets commands already being applied finish, persists event data, closes all sessions, and exits. Clients see an ordinary connection close.

### Conflict Table

In each row, A is the command the server applies first and B is applied second. Each row is also a test case to be handled during development.

| # | Scenario | A's result | B's result |
| --- | --- | --- | --- |
| 1 | Both `CLAIM_ITEM` the same item | `200 OK`, then `ITEM_CLAIMED` | receives A's `ITEM_CLAIMED`, then its own `410 Conflict: item already claimed by User1` |
| 2 | `CLAIM_ITEM` for different items in one event | `200 OK` | `200 OK` (both applied, in lock order) |
| 3 | `CLAIM_ITEM` in two different events | `200 OK` | `200 OK` (concurrent, independent) |
| 4 | Both `CREATE_EVENT` with the same code | `201 Created Event` | `410 Conflict: event code Party1 is already in use` |
| 5 | A `RELEASE_ITEM` and B `CLAIM_ITEM` of the same item (A is the claimer) | `200 OK`, then `ITEM_RELEASED` | `200 OK`, then `ITEM_CLAIMED` |
| 5b | Same as 5, but B's claim is applied first | B's `CLAIM_ITEM`: `410 Conflict: item already claimed by User1` | A's `RELEASE_ITEM`: `200 OK` |
| 6 | Non-claimer `RELEASE_ITEM` of a claimed item | n/a | `410 Conflict: Chips is claimed by another user` |
| 7 | `DELETE_EVENT` and a guest's `CLAIM_ITEM` or `RSVP`, delete applied first | `202 Deleted Event` | `401 Not Identified: not identified in event Party1` |
| 7b | Same as 7, but the guest's command is applied first | guest: `200 OK`, then the notification | organizer: `202 Deleted Event`; guest then receives `EVENT_DELETED` |
| 8 | Two `DELETE_EVENT` for the same event (e.g. two organizer connections) | `202 Deleted Event` | `401 Not Identified: not identified in event Party1` |
| 9 | `DELETE_EVENT`, then `CREATE_EVENT` with the same code, then an old guest sends `CLAIM_ITEM` | n/a | old guest: `401 Not Identified: not identified in event Party1` until it sends `JOIN` |
| 10 | `UPDATE_EVENT` and a new guest's `JOIN`, update applied first | `200 OK` | `200 OK`, confirmation shows the new date and time, no `EVENT_UPDATED` for this join |
| 10b | Same as 10, but `JOIN` is applied first | joiner: `200 OK`, confirmation shows the old date and time | organizer: `200 OK`; joiner receives `EVENT_UPDATED` |
| 11 | Two connections `JOIN` the same event as the same username | `200 OK` | `200 OK` (rejoin, each connection is independent) |
| 12 | One connection sends `JOIN` twice for the same event | `200 OK` | `410 Conflict: already identified in event Party1` |
| 13 | Client disconnects after command applied, before reading the response | change stays applied | other clients still receive the notification |
| 14 | Client stops reading until its output queue overflows | n/a | session ended, other clients unaffected |
| 15 | `RSVP` by two guests at once | `200 OK`, organizer notified | `200 OK`, organizer notified (two separate notifications, in applied order) |
| 16 | A guest's `RSVP No` and that same guest's `CLAIM_ITEM`, `RSVP No` applied first | `200 OK`, the guest's claimed items are released | `410 Conflict: you RSVP'd No; change your RSVP to Yes or Maybe to claim items` |
| 16b | Same as 16, but the `CLAIM_ITEM` is applied first | claim: `200 OK` | the later `RSVP No`: `200 OK`, and the new claim is released with the rest |

## Commands

Every error listed under a command's **Errors** is sent in the format described under Error response format: `<code> <phrase>: <reason>`.

### `JOIN`

The `JOIN` command identifies a user for an existing event.

**Client Request:**

```
JOIN <EventCode> <UserName>
```

All fields are required. `<EventCode>` must match an existing event, and `<UserName>` must satisfy the field rules in Message Encoding and Limits.

**Server Behavior (success):** The server records the identity `(EventCode -> UserName)` for this connection and returns a `200 OK` message, followed by a confirmation line that they are a guest of the event and shows the event's current date and time (a two-line response):

```
<UserName>, you are now a guest of <EventCode> (<Date> <Time>).
```

- If `<UserName>` is not yet a guest of the event, a new guest is added with an RSVP status of `Maybe`.
- If `<UserName>` is already a guest of the event, this is a rejoin: the guest keeps their existing RSVP status and the connection is identified as that guest.

**Example:**

```
c: JOIN Party1 User1
s: 200 OK
s: User1, you are now a guest of Party1 (09/22/2026 03:00PM).
```

**Errors:**

- `300 Message Format Error` - a field is missing, or the username/event code is malformed.
- `404 Not Found` - the event does not exist.
- `410 Conflict` - this connection is already identified in this event, whether under the same username or a different one.

> Note: there is no authentication, so joining with an existing username (including the organizer's) is treated as a rejoin of that user.

---

### `CREATE_EVENT`

The `CREATE_EVENT` command creates a new event. The client user who sends the command becomes the event's organizer and is automatically added as a guest of the event (with RSVP status `Maybe`) and identified for it on this connection.

**Client Request:**

```
CREATE_EVENT <EventCode> <UserName> <Date> <Time>
```

All fields are required. `<EventCode>` and `<UserName>` must satisfy the field rules in Message Encoding and Limits. `<Date>` must be a valid calendar date in the form MM/DD/YYYY, and `<Time>` must be a valid time in the form `HH:MMAM` or `HH:MMPM` (no space, hour 01-12, minute 00-59).

**Server Behavior (success):** The server returns a `201 Created Event` message, followed by a message that reiterates the code for the event (a two-line response).

**Example:**

```
c: CREATE_EVENT Party1 User1 09/22/2026 03:00PM
s: 201 Created Event
s: EventCode: Party1
```

`<EventCode>` must be unique among active events. Requests are handled in the order received, so if two organizers try to create an event with the same code, the first request is honored and the second receives a `410 Conflict`. If any field is misformatted or empty, the client receives a `300 Message Format Error`.

---

### `DELETE_EVENT`

The `DELETE_EVENT` command deletes an existing event. The client user who sends the command must be the creator of the event.

**Client Request:**

```
DELETE_EVENT <EventCode>
```

`<EventCode>` is required and must map to an active event that this user is the organizer of.

**Server Behavior (success):**

1. The server determines the recipients of the `EVENT_DELETED` notification (all connections identified in the event).
2. The event is removed.
3. The identity for this event is removed from every client session that held one (not just the requester's), so no connection retains access to the event code. This ensures that if the code is later reused by a new event, previously identified connections do not silently gain access to it and must `JOIN` again.
4. The server returns a `202 Deleted Event` message, and pushes a `210 <EventCode> EVENT_DELETED This event has been deleted` to the recipients determined in step 1.

Once deleted, the event code is free to be used by a new `CREATE_EVENT`.

**Example:**

```
c: DELETE_EVENT Party1
s: 202 Deleted Event

Notification to connections identified in the event:
210 Party1 EVENT_DELETED This event has been deleted
```

**Errors:** `300` if the event code is missing; `401 Not Identified` if the connection is not identified in the event (including an event that does not exist); `402 Not Authorized` if the user is a guest but not the organizer.

---

### `RSVP`

Allows an identified guest to confirm or update their attendance status. New guests start with a status of `Maybe`.

**Client Request:**

```
RSVP <EventCode> <Status>
```

`<EventCode>` must refer to an event the connection is identified in, and `<Status>` is required and must be exactly `Yes`, `No`, or `Maybe`.

**Server Behavior (success):** The server updates the status and returns a `200 OK` message, and sends a notification to the organizer in the form `210 <EventCode> RSVP_UPDATED <RSVPUser> status is now <Status>`.

**Effect of `No`:**

- When a user RSVPs `No`, the server releases every item that user currently claims in the event. Released items stay on the item list as unclaimed. The status change and the releases are applied together as one step.
- For each released item, the server sends `210 <EventCode> ITEM_RELEASED <ItemName> is available to bring again` to all connections identified in the event, after the response to the `RSVP`.
- While a user's status is `No`, that user's `CLAIM_ITEM` returns `410 Conflict`.
- Changing the status to `Yes` or `Maybe` allows claims again. Items released by RSVPing `No` are not restored.
- The status is saved with the guest, so a rejoin keeps it.
- `RELEASE_ITEM`, `UPDATE_EVENT`, and `DELETE_EVENT` are not affected by the user's RSVP status.
- This applies to every guest, including the organizer.

**Example (guest client):**

```
c: RSVP Party1 Yes
s: 200 OK

Notification to organizer client:
210 Party1 RSVP_UPDATED User2 status is now Yes
```

**Errors:** `300` if the status is not `Yes`, `No`, or `Maybe`; `401` if the connection is not identified in the event.

---

### `ITEMLIST`

The `ITEMLIST` command returns the list of items for an event and their claim status.

**Client Request:**

```
ITEMLIST <EventCode>
```

`<EventCode>` must refer to an event the connection is identified in.

**Server Behavior (success):** A `200 OK` message is returned, followed by one line per item, followed by the `END` terminator. An item is only added to the list when it is first claimed; if it is later released it remains on the list as unclaimed. The list is empty (just `200 OK` then `END`) if no item has ever been claimed.

**Example:**

```
c: ITEMLIST Party1
s: 200 OK
s: Chips - unclaimed
s: Soda - claimed by User1
s: END
```

**Errors:** `300` if the event code is missing; `401` if the connection is not identified in the event.

---

### `CLAIM_ITEM` / `RELEASE_ITEM`

The `CLAIM_ITEM`/`RELEASE_ITEM` commands allow a guest to claim/release an item for the event.

**Client Request:**

```
CLAIM_ITEM <EventCode> <ItemName>
```

OR

```
RELEASE_ITEM <EventCode> <ItemName>
```

`<EventCode>` must refer to an event the connection is identified in, and `<ItemName>` must satisfy the field rules in Message Encoding and Limits. Item names are unique within an event.

**`CLAIM_ITEM` behavior:**

- If the item is not on the list, it is created and claimed by the requesting user.
- If the item is on the list as unclaimed (it was previously released), it is claimed by the requesting user.
- If the item is on the list and already claimed (`claimedBy` is non-null), by anyone including the requester, the server returns `410 Conflict`.

A claim is rejected when the item is currently claimed, or when the requesting user's RSVP status is `No`. A released item can otherwise always be claimed again.

**`RELEASE_ITEM` behavior:** The item becomes unclaimed but stays on the list. If the item does not exist, is already unclaimed, or is claimed by someone else, the server returns `410 Conflict`.

**Server Behavior (success):** The server updates the item list, returns a `200 OK` message, and pushes a notification to all connections identified in the event: `210 <EventCode> ITEM_CLAIMED <ItemName> claimed by <UserName>` for a claim, or `210 <EventCode> ITEM_RELEASED <ItemName> is available to bring again` for a release.

**Example (guest client):**

```
c: CLAIM_ITEM Party1 Chips
s: 200 OK

Notification to all connections identified in the event:
210 Party1 ITEM_CLAIMED Chips claimed by User1
```

**Errors:** `300` if the item is malformed/empty; `401` if the connection is not identified in the event; `410` as described above.

---

### `UPDATE_EVENT` *(organizer only)*

This command allows the organizer to edit event details. The user must be the creator of the event.

**Client Request:**

```
UPDATE_EVENT <EventCode> <Date> <Time>
```

`<EventCode>` must refer to an existing event the user is the organizer of, `<Date>` must be a valid calendar date in the form MM/DD/YYYY, and `<Time>` must be a valid time in the form `HH:MMAM` or `HH:MMPM`. For simplicity, both Date and Time must be present for an update.

**Server Behavior (success):** The server returns a `200 OK` message, and pushes a `210 <EventCode> EVENT_UPDATED new event details: <Date> <Time>` to all connections identified in the event.

**Example (organizer client):**

```
c: UPDATE_EVENT Party1 10/15/2026 06:00PM
s: 200 OK

Notification to all connections identified in the event:
210 Party1 EVENT_UPDATED new event details: 10/15/2026 06:00PM
```

**Errors:** `300` if the date/time is not formatted correctly; `401` if the connection is not identified in the event; `402 Not Authorized` if the user is a guest but not the organizer.

---

### `WHO`

The `WHO` command lets the user know who is attending the event.

**Client Request:**

```
WHO <EventCode>
```

`<EventCode>` must refer to an event the connection is identified in.

**Server Behavior (success):** A `200 OK` message is returned, followed by one line per guest with their RSVP status (the organizer is marked), followed by the `END` terminator. Guests who have not yet RSVP'd show `Maybe`.

**Example:**

```
c: WHO Party1
s: 200 OK
s: User1 (organizer) - Yes
s: User2 - Maybe
s: END
```

**Errors:** `300` if the event code is missing; `401` if the connection is not identified in the event.

---

### `QUIT`

If a user issues a `QUIT` command, it terminates the current client connection without shutting down the server.

**Client Request:**

```
QUIT
```

**Server Behavior:** Closes the requesting client's socket and removes that client's session (including all of its event identities). Guest records remain in their events, so the user may later `JOIN` again. The server remains active and continues serving other clients. No response is sent. An abrupt disconnect is handled the same way as `QUIT`.

---

### `HELP` *(client side only)*

`HELP` is purely for the user to understand all of the commands they are able to send, and does not contact the server. The same menu is displayed by the client automatically after connecting.

## General Protocol Requirements

- Every command and response line must end with `\n`. Text is UTF-8 and lines are limited to 527 bytes (see Message Encoding and Limits).
- Both client and server must buffer partial reads and only process complete lines (see Receiver-Side Buffering).
- The server tracks identity per connection. A connection may be identified in multiple events, with a (possibly different) username in each.
- Identities are removed from all sessions when an event is deleted, and when a connection closes.
- Requests are handled in the order received. Changes to a single event are applied one at a time (first come, first served), while different events may be modified concurrently.
- Responses are written as uninterrupted blocks, and notifications are sent only between response blocks (see Message Atomicity and Ordering).
- The server sends messages to relevant clients when event information is changed, and the client must be able to receive and display these messages asynchronously.
- Event data is persisted by writing it to storage on every change, so events survive a server failure. Client sessions are not persisted.
- There is no root/administrative role, and stopping the server process is done externally when needed (see Graceful Shutdown).
