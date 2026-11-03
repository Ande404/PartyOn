#!/usr/bin/env python3
"""PartyOn protocol conformance tests against a running server.

Sends raw bytes over TCP (no client involved) and checks every response
against docs/UpdatedProtocolSpecification.md. Test IDs match the Phase 1
test plan (TC-F*) plus response-code (RC-*) and behaviour (TC-B*) cases.

Usage:
    ./bin/partyon-server &                 # start the server first
    python3 tests/protocol_test.py [host] [port]    # defaults: 127.0.0.1 5527

The server is Level 1 (one client at a time), so every test closes its
connection before the next one starts. Event codes get a per-run suffix,
so the script can be re-run against the same server.
"""

import socket
import sys
import time
import uuid

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5527
RUN = uuid.uuid4().hex[:6]

DATE, TIME = "10/31/2026", "07:00PM"


def ev(name):
    """Event code unique to this run."""
    return f"{name}_{RUN}"


class Conn:
    """One TCP connection with line buffering and timeouts."""

    def __init__(self):
        self.sock = socket.create_connection((HOST, PORT), timeout=3)
        self.buf = b""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send(self, data):
        self.sock.sendall(data if isinstance(data, bytes) else data.encode())

    def line(self, timeout=2.0):
        """Next complete line without '\\n', or None on timeout/close."""
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        return raw.decode("utf-8", "replace")

    def lines(self, count, timeout=2.0):
        return [self.line(timeout) for _ in range(count)]

    def silent(self, wait=0.3):
        """True when nothing more arrives within `wait` seconds."""
        return self.line(wait) is None

    def cmd(self, text, count=1):
        self.send(text + "\n")
        return self.lines(count)

    def closed_by_server(self, wait=1.0):
        self.sock.settimeout(wait)
        try:
            return self.sock.recv(1) == b""
        except socket.timeout:
            return False


RESULTS = []


def test(test_id, description):
    def wrap(fn):
        RESULTS.append((test_id, description, fn))
        return fn
    return wrap


def bare(value):
    """Drop the ": reason" part of error lines so checks compare status codes."""
    if isinstance(value, str):
        if len(value) > 4 and value[0] in "34" and value[1:3].isdigit() and ": " in value:
            return value.split(": ", 1)[0]
        return value
    if isinstance(value, (list, tuple)):
        return type(value)(bare(v) for v in value)
    return value


def check(actual, expected):
    """Compare responses, ignoring error reasons (see check_exact)."""
    if bare(actual) != expected:
        raise AssertionError(f"expected {expected!r}, got {actual!r}")


def check_exact(actual, expected):
    if actual != expected:
        raise AssertionError(f"expected {expected!r}, got {actual!r}")


def create(c, code, user="Andrew", date=DATE, time_=TIME):
    check(c.cmd(f"CREATE_EVENT {code} {user} {date} {time_}", 2),
          ["201 Created Event", f"EventCode: {code}"])


# ---------------------------------------------------------------- framing

@test("TC-F01", "One command in one write is parsed and executed once")
def _():
    with Conn() as c:
        code = ev("F01")
        create(c, code)
        assert c.silent()


@test("TC-F02", "One command sent byte by byte")
def _():
    with Conn() as c:
        code = ev("F02")
        for b in f"CREATE_EVENT {code} Andrew {DATE} {TIME}\n".encode():
            c.send(bytes([b]))
            time.sleep(0.002)
        check(c.lines(2), ["201 Created Event", f"EventCode: {code}"])
        assert c.silent()


@test("TC-F03", "Three commands in one write are answered in order")
def _():
    with Conn() as c:
        code = ev("F03")
        create(c, code)
        c.send(f"WHO {code}\nITEMLIST {code}\nRSVP {code} Purple\n")
        check(c.lines(6), ["200 OK", "Andrew (organizer) - Maybe", "END",
                           "200 OK", "END", "300 Message Format Error"])


@test("TC-F04", "Command split mid-token is reassembled")
def _():
    with Conn() as c:
        code = ev("F04")
        create(c, code)
        command = f"WHO {code}\n"
        c.send(command[:2])           # "WH"
        time.sleep(0.2)
        c.send(command[2:7])          # "O " plus the start of the code
        time.sleep(0.2)
        c.send(command[7:])
        check(c.lines(3), ["200 OK", "Andrew (organizer) - Maybe", "END"])


@test("TC-F05", "Multi-byte UTF-8 character split across two writes")
def _():
    with Conn() as c:
        code = ev("F05")
        name = "José".encode()  # é is C3 A9
        c.send(b"CREATE_EVENT " + code.encode() + b" " + name[:-1])
        time.sleep(0.2)
        c.send(name[-1:] + f" {DATE} {TIME}\n".encode())
        check(c.lines(2), ["201 Created Event", f"EventCode: {code}"])
        check(c.cmd(f"WHO {code}", 3), ["200 OK", "José (organizer) - Maybe", "END"])


@test("TC-F06", "\\r\\n line endings are accepted")
def _():
    with Conn() as c:
        code = ev("F06")
        c.send(f"CREATE_EVENT {code} Andrew {DATE} {TIME}\r\n")
        check(c.lines(2), ["201 Created Event", f"EventCode: {code}"])


@test("TC-F07", "Empty line gets 300 (team decision)")
def _():
    with Conn() as c:
        check(c.cmd(""), ["300 Message Format Error"])


@test("TC-F08", "Connection closed mid-line: partial command is not executed")
def _():
    code = ev("F08")
    with Conn() as c:
        create(c, code)
        c.send(f"DELETE_EVENT {code}")  # no newline
    time.sleep(0.3)
    with Conn() as c:
        check(c.cmd(f"JOIN {code} Sam", 2)[0], "200 OK")


@test("TC-F09", "Line over 527 bytes gets 300 and the next line still works")
def _():
    with Conn() as c:
        c.send(b"A" * 600 + b"\n")
        check(c.line(), "300 Message Format Error")
        c.send(b"B" * 400)
        time.sleep(0.2)
        c.send(b"B" * 400)  # now over the limit with no newline yet
        check(c.line(), "300 Message Format Error")
        c.send(b"tail of the long line\n")
        code = ev("F09")
        create(c, code)
        # The exact 527/528-byte boundary is covered by tests/test_framer.cpp


# ---------------------------------------------------------- response codes

@test("RC-200", "Valid RSVP returns 200 OK")
def _():
    with Conn() as c:
        code = ev("R200")
        create(c, code)
        check(c.cmd(f"RSVP {code} Yes", 2),
              ["200 OK", f"210 {code} RSVP_UPDATED Andrew status is now Yes"])


@test("RC-201", "Valid CREATE_EVENT returns 201 and the event code")
def _():
    with Conn() as c:
        create(c, ev("R201"))


@test("RC-202", "Organizer DELETE_EVENT returns 202 then EVENT_DELETED")
def _():
    with Conn() as c:
        code = ev("R202")
        create(c, code)
        check(c.cmd(f"DELETE_EVENT {code}", 2),
              ["202 Deleted Event", f"210 {code} EVENT_DELETED This event has been deleted"])


@test("RC-210", "CLAIM_ITEM: 200 OK first, then the ITEM_CLAIMED notification")
def _():
    with Conn() as c:
        code = ev("R210")
        create(c, code)
        check(c.cmd(f"CLAIM_ITEM {code} Chips", 2),
              ["200 OK", f"210 {code} ITEM_CLAIMED Chips claimed by Andrew"])
        check(c.cmd(f"RELEASE_ITEM {code} Chips", 2),
              ["200 OK", f"210 {code} ITEM_RELEASED Chips is available to bring again"])
        check(c.cmd(f"UPDATE_EVENT {code} 11/01/2026 06:30PM", 2),
              ["200 OK", f"210 {code} EVENT_UPDATED new event details: 11/01/2026 06:30PM"])


@test("RC-300", "Malformed commands return 300")
def _():
    with Conn() as c:
        code = ev("R300")
        create(c, code)
        for bad in [f"RSVP {code} Purple", "DANCE", f"who {code}", f"WHO  {code}",
                    f"WHO {code} extra", "HELP", "QUIT now",
                    f"CREATE_EVENT X U 02/30/2026 {TIME}", f"CREATE_EVENT X U {DATE} 13:00PM",
                    f"JOIN {code} " + "u" * 101]:
            check((bad, c.cmd(bad)), (bad, ["300 Message Format Error"]))
        c.send(b"JOIN " + code.encode() + b" Bad\xff\n")
        check(c.line(), "300 Message Format Error")


@test("RC-ERR", "Error lines carry a reason: \"<code> <phrase>: <reason>\"")
def _():
    with Conn() as c:
        code = ev("ERR")
        create(c, code)
        check_exact(c.cmd(f"RSVP {code} Purple"),
                    ["300 Message Format Error: invalid RSVP status in field 2 (expected Yes, No, or Maybe)"])
        check_exact(c.cmd(f"UPDATE_EVENT {code} 02/30/2026 06:00PM"),
                    ["300 Message Format Error: invalid date in field 2 (expected a real date as MM/DD/YYYY)"])
        check_exact(c.cmd(f"CLAIM_ITEM {code}"),
                    ["300 Message Format Error: wrong number of fields (got 1), "
                     "usage: CLAIM_ITEM <EventCode> <ItemName>"])
        check_exact(c.cmd(f"WHO {ev('none')}"),
                    ["401 Not Identified: this connection has not joined that event"])
        check_exact(c.cmd(f"JOIN {ev('none')} Sam"), ["404 Not Found: no event with that code"])
        c.cmd(f"CLAIM_ITEM {code} Cake", 2)
        check_exact(c.cmd(f"CLAIM_ITEM {code} Cake"), ["410 Conflict: item already claimed by Andrew"])
        for line in c.cmd(f"RSVP {code} Purple") + c.cmd("DANCE"):
            assert len(line.encode()) + 1 <= 527, "error line over the 527-byte limit"


@test("RC-401", "RSVP before JOIN returns 401; 300 takes precedence")
def _():
    with Conn() as c:
        check(c.cmd(f"RSVP {ev('R401')} Yes"), ["401 Not Identified"])
        check(c.cmd(f"RSVP {ev('R401')} Purple"), ["300 Message Format Error"])


@test("RC-402", "Guest UPDATE_EVENT / DELETE_EVENT returns 402")
def _():
    code = ev("R402")
    with Conn() as c:
        create(c, code)
    time.sleep(0.2)
    with Conn() as c:
        check(c.cmd(f"JOIN {code} Sam", 2)[0], "200 OK")
        check(c.cmd(f"UPDATE_EVENT {code} 11/01/2026 06:30PM"), ["402 Not Authorized"])
        check(c.cmd(f"DELETE_EVENT {code}"), ["402 Not Authorized"])


@test("RC-404", "JOIN to a non-existent event returns 404")
def _():
    with Conn() as c:
        check(c.cmd(f"JOIN {ev('nope')} Sam"), ["404 Not Found"])


@test("RC-410", "Duplicate event code, claimed item, double JOIN return 410")
def _():
    with Conn() as c:
        code = ev("R410")
        create(c, code)
        check(c.cmd(f"CREATE_EVENT {code} Kim {DATE} {TIME}"), ["410 Conflict"])
        c.cmd(f"CLAIM_ITEM {code} Chips", 2)
        check(c.cmd(f"CLAIM_ITEM {code} Chips"), ["410 Conflict"])
        check(c.cmd(f"RELEASE_ITEM {code} Soda"), ["410 Conflict"])
        check(c.cmd(f"JOIN {code} Andrew"), ["410 Conflict"])


# ---------------------------------------------------------------- behaviour

@test("TC-B01", "JOIN reply includes the event date and time (FR-05)")
def _():
    code = ev("B01")
    with Conn() as c:
        create(c, code)
    time.sleep(0.2)
    with Conn() as c:
        check(c.cmd(f"JOIN {code} Sam", 2),
              ["200 OK", f"Sam, you are now a guest of {code} ({DATE} {TIME})."])


@test("TC-B02", "Rejoin with the same name keeps the RSVP status")
def _():
    code = ev("B02")
    with Conn() as c:
        create(c, code)
    time.sleep(0.2)
    with Conn() as c:
        c.cmd(f"JOIN {code} Sam", 2)
        check(c.cmd(f"RSVP {code} No"), ["200 OK"])
    time.sleep(0.2)
    with Conn() as c:
        check(c.cmd(f"JOIN {code} Sam", 2)[0], "200 OK")
        check(c.cmd(f"WHO {code}", 4),
              ["200 OK", "Andrew (organizer) - Maybe", "Sam - No", "END"])


@test("TC-B03", "Deleted event: 401 for commands, 404 for JOIN, code reusable")
def _():
    with Conn() as c:
        code = ev("B03")
        create(c, code)
        c.cmd(f"DELETE_EVENT {code}", 2)
        check(c.cmd(f"WHO {code}"), ["401 Not Identified"])
        check(c.cmd(f"JOIN {code} Sam"), ["404 Not Found"])
        create(c, code, user="Kim")


@test("TC-B04", "QUIT: no response, server closes the socket and keeps running")
def _():
    with Conn() as c:
        c.send("QUIT\n")
        assert c.closed_by_server(), "server did not close the connection"
    time.sleep(0.2)
    with Conn() as c:
        create(c, ev("B04"))


@test("TC-B05", "One connection identified in two events with different names")
def _():
    with Conn() as c:
        a, b = ev("B05a"), ev("B05b")
        create(c, a, user="Andrew")
        create(c, b, user="Drew")
        check(c.cmd(f"WHO {b}", 3), ["200 OK", "Drew (organizer) - Maybe", "END"])
        check(c.cmd(f"CLAIM_ITEM {a} Cake", 2)[1], f"210 {a} ITEM_CLAIMED Cake claimed by Andrew")


def main():
    print(f"PartyOn protocol tests against {HOST}:{PORT} (run {RUN})\n")
    failures = 0
    for test_id, description, fn in RESULTS:
        try:
            fn()
            print(f"PASS  {test_id:8} {description}")
        except Exception as error:  # noqa: BLE001 - report every failure
            failures += 1
            print(f"FAIL  {test_id:8} {description}\n      {error}")
    print(f"\n{len(RESULTS) - failures}/{len(RESULTS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
