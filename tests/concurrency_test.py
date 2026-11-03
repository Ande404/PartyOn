#!/usr/bin/env python3
"""PartyOn multi-client tests against a running server (protocol spec v2).

Several real TCP clients send commands at the same moment (a barrier releases
them together) to check first-come-first-served conflict handling,
notification fan-out, failure isolation, and slow-client protection. Test IDs
map to the spec v2 Conflict Table rows (CT-*) and the Phase 1 test plan
(TC-C*).

Usage:
    ./bin/partyon-server &
    python3 tests/concurrency_test.py [host] [port]     # defaults: 127.0.0.1 5527
"""

import socket
import sys
import threading
import time
import uuid

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5527
RUN = uuid.uuid4().hex[:6]
DATE, TIME = "10/31/2026", "07:00PM"
NEW_DATE, NEW_TIME = "11/01/2026", "06:30PM"


def ev(name):
    return f"{name}_{RUN}"


def code_of(line):
    return line[:3] if line else None


class Conn:
    """One client connection with line buffering."""

    def __init__(self, rcvbuf=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(5)
        self.sock.connect((HOST, PORT))
        self.buf = b""
        self.notes = []  # notifications that arrived before a reply

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send(self, text):
        self.sock.sendall(text.encode() if isinstance(text, str) else text)

    def line(self, timeout=3.0):
        deadline = time.time() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except (socket.timeout, ConnectionResetError):
                return None
            if not chunk:
                return None
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        return raw.decode("utf-8", "replace")

    def lines(self, count, timeout=3.0):
        return [self.line(timeout) for _ in range(count)]

    def cmd(self, text, count=1):
        """Send a command and return its reply lines.

        Notifications caused by other clients may arrive before the reply;
        they are kept for drain(). Lines inside a reply are never notifications.
        """
        self.send(text + "\n")
        first = self.line()
        while first is not None and first.startswith("210 "):
            self.notes.append(first)
            first = self.line()
        return [first] + self.lines(count - 1)

    def drain(self, wait=0.4):
        """Notifications set aside by cmd(), plus every line until `wait` seconds of quiet."""
        out, self.notes = self.notes, []
        while True:
            line = self.line(wait)
            if line is None:
                return out
            out.append(line)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def at_once(*actions):
    """Run the callables in threads released together; return their results."""
    barrier = threading.Barrier(len(actions))
    results = [None] * len(actions)
    errors = []

    def run(i, action):
        try:
            barrier.wait()
            results[i] = action()
        except Exception as error:  # noqa: BLE001
            errors.append(error)

    threads = [threading.Thread(target=run, args=(i, a)) for i, a in enumerate(actions)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise errors[0]
    return results


def party(code, guests):
    """Organizer connection plus one connection per guest, all joined to `code`."""
    org = Conn()
    check(org.cmd(f"CREATE_EVENT {code} Host {DATE} {TIME}", 2)[0] == "201 Created Event", "create failed")
    conns = []
    for name in guests:
        c = Conn()
        check(c.cmd(f"JOIN {code} {name}", 2)[0] == "200 OK", f"join {name} failed")
        conns.append(c)
    return org, conns


def close_all(*conns):
    for c in conns:
        c.close()


RESULTS = []


def test(test_id, description):
    def wrap(fn):
        RESULTS.append((test_id, description, fn))
        return fn
    return wrap


# ------------------------------------------------------------------ races

def claim_race(clients):
    code = ev(f"race{clients}")
    org, guests = party(code, [f"G{i}" for i in range(clients)])
    try:
        replies = at_once(*[lambda c=c: c.cmd(f"CLAIM_ITEM {code} Cake") for c in guests])
        codes = [code_of(r[0]) for r in replies]
        check(codes.count("200") == 1, f"expected one winner, got {codes}")
        check(codes.count("410") == clients - 1, f"expected {clients - 1} conflicts, got {codes}")
        winner = f"G{codes.index('200')}"
        expected = f"210 {code} ITEM_CLAIMED Cake claimed by {winner}"
        # Every identified connection, winners and losers and the organizer, sees the same winner
        for c in guests + [org]:
            got = [l for l in c.drain() if l.startswith("210 ")]
            check(got == [expected], f"expected [{expected!r}], got {got}")
        for i, r in enumerate(replies):
            if code_of(r[0]) == "410":
                check(r[0] == f"410 Conflict: item already claimed by {winner}", r[0])
    finally:
        close_all(org, *guests)


@test("TC-C01", "2 clients claim the same item: one 200, one 410 (CT-1)")
def _():
    claim_race(2)


@test("TC-C02", "3 and then 5 clients claim the same item: exactly one winner")
def _():
    claim_race(3)
    claim_race(5)


@test("TC-C03", "All connections are notified of the same winner")
def _():
    claim_race(4)  # claim_race checks every connection's notification


@test("CT-2", "Different items in one event, at once: both 200")
def _():
    code = ev("ct2")
    org, (a, b) = party(code, ["A", "B"])
    try:
        ra, rb = at_once(lambda: a.cmd(f"CLAIM_ITEM {code} Chips"), lambda: b.cmd(f"CLAIM_ITEM {code} Soda"))
        check(code_of(ra[0]) == "200" and code_of(rb[0]) == "200", f"{ra} {rb}")
        a.drain()
        check(sorted(a.cmd(f"ITEMLIST {code}", 4)[1:3]) == ["Chips - claimed by A", "Soda - claimed by B"], "itemlist")
    finally:
        close_all(org, a, b)


@test("TC-C06", "Two clients modify different events at once: both succeed (CT-3)")
def _():
    with Conn() as a, Conn() as b:
        ca, cb = ev("ct3a"), ev("ct3b")
        a.cmd(f"CREATE_EVENT {ca} A {DATE} {TIME}", 2)
        b.cmd(f"CREATE_EVENT {cb} B {DATE} {TIME}", 2)
        ra, rb = at_once(lambda: a.cmd(f"CLAIM_ITEM {ca} Cake", 2), lambda: b.cmd(f"CLAIM_ITEM {cb} Cake", 2))
        check(ra[0] == "200 OK" and rb[0] == "200 OK", f"{ra} {rb}")


@test("TC-C04", "Two clients CREATE_EVENT the same code at once: one 201, one 410 (CT-4)")
def _():
    code = ev("ct4")
    with Conn() as a, Conn() as b:
        ra, rb = at_once(lambda: a.cmd(f"CREATE_EVENT {code} A {DATE} {TIME}"),
                         lambda: b.cmd(f"CREATE_EVENT {code} B {DATE} {TIME}"))
        codes = sorted([code_of(ra[0]), code_of(rb[0])])
        check(codes == ["201", "410"], f"got {codes}")


@test("TC-C05", "RELEASE by the claimer races CLAIM by another guest (CT-5 / CT-5b)")
def _():
    code = ev("ct5")
    org, (a, b) = party(code, ["A", "B"])
    try:
        a.cmd(f"CLAIM_ITEM {code} Cake", 2)
        b.drain()
        rel, cla = at_once(lambda: a.cmd(f"RELEASE_ITEM {code} Cake"), lambda: b.cmd(f"CLAIM_ITEM {code} Cake"))
        check(code_of(rel[0]) == "200", f"release: {rel}")
        a.drain(); b.drain()
        items = a.cmd(f"ITEMLIST {code}", 3)
        if code_of(cla[0]) == "200":      # CT-5: release applied first
            check(items[1] == "Cake - claimed by B", f"{items}")
        else:                             # CT-5b: claim applied first and lost
            check(code_of(cla[0]) == "410", f"claim: {cla}")
            check(items[1] == "Cake - unclaimed", f"{items}")
    finally:
        close_all(org, a, b)


@test("CT-6", "Non-claimer RELEASE_ITEM of a claimed item: 410")
def _():
    code = ev("ct6")
    org, (a, b) = party(code, ["A", "B"])
    try:
        a.cmd(f"CLAIM_ITEM {code} Cake", 2)
        b.drain()
        check(b.cmd(f"RELEASE_ITEM {code} Cake")[0] == "410 Conflict: only A can release that item", "release")
    finally:
        close_all(org, a, b)


@test("CT-7", "DELETE_EVENT races a guest's CLAIM_ITEM (CT-7 / CT-7b)")
def _():
    code = ev("ct7")
    org, (g,) = party(code, ["G"])
    try:
        dele, cla = at_once(lambda: org.cmd(f"DELETE_EVENT {code}"), lambda: g.cmd(f"CLAIM_ITEM {code} Cake"))
        check(dele[0] == "202 Deleted Event", f"delete: {dele}")
        rest = g.drain()
        deleted = f"210 {code} EVENT_DELETED This event has been deleted"
        # CT-7: delete applied first, so the claim gets 401. CT-7b: the claim wins
        # with 200 OK. Either way the guest was identified at the delete and is notified.
        check(code_of(cla[0]) in ("200", "401"), f"claim: {cla}")
        check(deleted in rest, f"no EVENT_DELETED: {rest}")
        check(code_of(g.cmd(f"WHO {code}")[0]) == "401", "event should be gone")
    finally:
        close_all(org, g)


@test("CT-8", "Two organizer connections DELETE the same event: 202 and 401")
def _():
    code = ev("ct8")
    org, (twin,) = party(code, ["Host"])  # second connection rejoins as the organizer
    try:
        r1, r2 = at_once(lambda: org.cmd(f"DELETE_EVENT {code}"), lambda: twin.cmd(f"DELETE_EVENT {code}"))
        codes = sorted([code_of(r1[0]), code_of(r2[0])])
        check(codes == ["202", "401"], f"got {r1} {r2}")
    finally:
        close_all(org, twin)


@test("CT-9", "Delete, re-create the code, old guest gets 401 until it JOINs again")
def _():
    code = ev("ct9")
    org, (g,) = party(code, ["G"])
    try:
        org.cmd(f"DELETE_EVENT {code}", 2)
        g.drain()
        with Conn() as other:
            other.cmd(f"CREATE_EVENT {code} New {DATE} {TIME}", 2)
            check(code_of(g.cmd(f"CLAIM_ITEM {code} Cake")[0]) == "401", "old guest kept access")
            check(g.cmd(f"JOIN {code} G", 2)[0] == "200 OK", "rejoin")
    finally:
        close_all(org, g)


@test("CT-10", "UPDATE_EVENT races a new guest's JOIN (CT-10 / CT-10b)")
def _():
    code = ev("ct10")
    org, _ = party(code, [])
    try:
        with Conn() as newcomer:
            upd, join = at_once(lambda: org.cmd(f"UPDATE_EVENT {code} {NEW_DATE} {NEW_TIME}"),
                                lambda: newcomer.cmd(f"JOIN {code} Late", 2))
            check(code_of(upd[0]) == "200", f"update: {upd}")
            rest = newcomer.drain()
            notice = f"210 {code} EVENT_UPDATED new event details: {NEW_DATE} {NEW_TIME}"
            if f"({NEW_DATE} {NEW_TIME})" in join[1]:       # CT-10: update first, no notification
                check(notice not in rest, f"unexpected {rest}")
            else:                                           # CT-10b: join first, then notified
                check(f"({DATE} {TIME})" in join[1] and notice in rest, f"{join} {rest}")
    finally:
        org.close()


@test("CT-11", "Two connections JOIN as the same username: both 200, both notified")
def _():
    code = ev("ct11")
    org, (a, b) = party(code, ["Sam", "Sam"])
    try:
        a.cmd(f"CLAIM_ITEM {code} Cake", 2)
        notice = f"210 {code} ITEM_CLAIMED Cake claimed by Sam"
        check(notice in b.drain(), "second Sam connection not notified")
    finally:
        close_all(org, a, b)


@test("CT-12", "One connection sends JOIN twice: 200 then 410")
def _():
    code = ev("ct12")
    org, (g,) = party(code, ["G"])
    try:
        check(code_of(g.cmd(f"JOIN {code} G")[0]) == "410", "second join")
    finally:
        close_all(org, g)


@test("CT-13", "Client disconnects right after a command: change stays, others notified")
def _():
    code = ev("ct13")
    org, (watcher,) = party(code, ["Watcher"])
    try:
        quitter = Conn()
        quitter.cmd(f"JOIN {code} Quitter", 2)
        quitter.send(f"CLAIM_ITEM {code} Cake\n")
        quitter.close()  # do not read the response
        check(f"210 {code} ITEM_CLAIMED Cake claimed by Quitter" in watcher.drain(1.0), "no notification")
        check(watcher.cmd(f"ITEMLIST {code}", 3)[1] == "Cake - claimed by Quitter", "claim lost")
    finally:
        close_all(org, watcher)


@test("CT-14", "A client that stops reading is dropped; other clients are unaffected")
def _():
    code = ev("ct14")
    org, (fast,) = party(code, ["Fast"])
    org.close()  # only the fast and the slow client stay connected
    slow = Conn(rcvbuf=4096)
    try:
        slow.cmd(f"JOIN {code} Slow", 2)
        # Generate far more notifications than the slow client's 1000-line queue
        # plus its socket buffers can hold; the fast client keeps reading
        batch = 100
        for _ in range(120):
            cmds = "".join(f"CLAIM_ITEM {code} X\nRELEASE_ITEM {code} X\n" for _ in range(batch))
            fast.send(cmds)
            got = fast.lines(batch * 4, timeout=5)  # 2 responses + 2 notifications each
            check(None not in got, "fast client stalled")
        check(fast.cmd(f"WHO {code}", 1)[0] == "200 OK", "fast client affected")
        fast.drain(0.2)
        # The slow client's connection was closed by the server
        slow.sock.settimeout(10)
        closed = False
        try:
            while True:
                chunk = slow.sock.recv(1 << 20)
                if not chunk:
                    closed = True
                    break
        except (ConnectionResetError, socket.timeout, OSError) as error:
            closed = not isinstance(error, socket.timeout)
        check(closed, "slow client was not disconnected")
    finally:
        close_all(org, fast, slow)


@test("CT-15", "Two guests RSVP at once: organizer gets both notifications")
def _():
    code = ev("ct15")
    org, (a, b) = party(code, ["A", "B"])
    try:
        ra, rb = at_once(lambda: a.cmd(f"RSVP {code} Yes"), lambda: b.cmd(f"RSVP {code} No"))
        check(ra[0] == "200 OK" and rb[0] == "200 OK", f"{ra} {rb}")
        got = sorted(org.drain())
        check(got == [f"210 {code} RSVP_UPDATED A status is now Yes",
                      f"210 {code} RSVP_UPDATED B status is now No"], f"{got}")
        check(a.drain() == [] and b.drain() == [], "guests should not get RSVP_UPDATED")
    finally:
        close_all(org, a, b)


# ------------------------------------------- last-minute cancellation

@test("TC-V01", "RSVP No releases the guest's items and every connection is told")
def _():
    code = ev("cancel")
    org, (sam, kim) = party(code, ["Sam", "Kim"])
    try:
        sam.cmd(f"CLAIM_ITEM {code} Cake", 2)
        sam.cmd(f"CLAIM_ITEM {code} Chips", 2)
        kim.cmd(f"CLAIM_ITEM {code} Soda", 2)
        for c in (org, sam, kim):
            c.drain(0.2)
        check(sam.cmd(f"RSVP {code} No")[0] == "200 OK", "rsvp")
        released = [f"210 {code} ITEM_RELEASED Cake is available to bring again",
                    f"210 {code} ITEM_RELEASED Chips is available to bring again"]
        check(org.drain() == [f"210 {code} RSVP_UPDATED Sam status is now No"] + released, "organizer view")
        check(kim.drain() == released, "other guest view")
        check(kim.cmd(f"ITEMLIST {code}", 5)[1:] == ["Cake - unclaimed", "Chips - unclaimed",
                                                     "Soda - claimed by Kim", "END"], "itemlist")
    finally:
        close_all(org, sam, kim)


@test("TC-V02", "RSVP No races another guest claiming the same item: one consistent outcome")
def _():
    code = ev("cancelrace")
    org, (sam, kim) = party(code, ["Sam", "Kim"])
    try:
        sam.cmd(f"CLAIM_ITEM {code} Cake", 2)
        kim.drain(0.2)
        rsvp, claim = at_once(lambda: sam.cmd(f"RSVP {code} No"), lambda: kim.cmd(f"CLAIM_ITEM {code} Cake"))
        check(rsvp[0] == "200 OK", f"rsvp: {rsvp}")
        kim.drain(0.2)
        owner = kim.cmd(f"ITEMLIST {code}", 3)[1]
        if claim[0] == "200 OK":           # cancellation applied first, Kim took the cake
            check(owner == "Cake - claimed by Kim", owner)
        else:                              # Kim's claim applied first and lost to Sam
            check(claim[0] == "410 Conflict: item already claimed by Sam", f"claim: {claim}")
            check(owner == "Cake - unclaimed", owner)
    finally:
        close_all(org, sam, kim)


# ---------------------------------------------------------- isolation

@test("TC-I01", "Malformed input and abrupt disconnects do not affect other clients")
def _():
    code = ev("iso")
    org, (a, b) = party(code, ["A", "B"])
    try:
        b.send(b"A" * 800 + b"\n")                        # oversized
        check(b.line().startswith("300 Message Format Error"), "oversized")
        b.send(b"JOIN " + code.encode() + b" Bad\xff\n")  # invalid UTF-8
        check(b.line().startswith("300 Message Format Error"), "utf8")
        b.send(f"CLAIM_ITEM {code} Hal")                   # partial line, then vanish
        b.close()
        check(a.cmd(f"CLAIM_ITEM {code} Cake", 2)[0] == "200 OK", "A affected")
        check(org.cmd(f"ITEMLIST {code}", 3)[1:] == ["Cake - claimed by A", "END"], "partial line was applied")
    finally:
        close_all(org, a, b)


@test("TC-I02", "A client stuck mid-line does not block other clients")
def _():
    with Conn() as stuck, Conn() as other:
        stuck.send("CREATE_EVENT Half")                    # never finished
        code = ev("stuck")
        start = time.time()
        check(other.cmd(f"CREATE_EVENT {code} X {DATE} {TIME}", 2)[0] == "201 Created Event", "blocked")
        check(time.time() - start < 1.0, "slow response while another client is mid-line")


@test("TC-I03", "Ten clients connected at once, each served")
def _():
    conns = [Conn() for _ in range(10)]
    try:
        replies = at_once(*[lambda c=c, i=i: c.cmd(f"CREATE_EVENT {ev('ten' + str(i))} U {DATE} {TIME}", 2)
                            for i, c in enumerate(conns)])
        check(all(r[0] == "201 Created Event" for r in replies), f"{replies}")
    finally:
        close_all(*conns)


def main():
    print(f"PartyOn concurrency tests against {HOST}:{PORT} (run {RUN})\n")
    failures = 0
    for test_id, description, fn in RESULTS:
        try:
            start = time.time()
            fn()
            print(f"PASS  {test_id:8} {description}  ({time.time() - start:.2f}s)")
        except Exception as error:  # noqa: BLE001
            failures += 1
            print(f"FAIL  {test_id:8} {description}\n      {error}")
    print(f"\n{len(RESULTS) - failures}/{len(RESULTS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
