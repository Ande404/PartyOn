#!/usr/bin/env python3
"""PartyOn persistence and restart tests (protocol spec v2, Persistence and
Graceful Shutdown; Phase 1 test plan section 9.6).

Starts the server itself on a free port with a temporary data directory,
builds up event state, stops the server (crash-style kill, or Ctrl+C), starts
it again, and checks that every acknowledged change survived.

Usage:
    python3 tests/persistence_test.py [path/to/partyon-server]
"""

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SERVER = os.path.join(ROOT, "bin", "partyon-server" + (".exe" if os.name == "nt" else ""))
SERVER = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SERVER


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """A partyon-server process using one data directory."""

    def __init__(self, workdir, port):
        self.workdir, self.port, self.proc = workdir, port, None

    def start(self):
        self.proc = subprocess.Popen([SERVER, str(self.port), "--data-dir", "data"], cwd=self.workdir,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start")

    def crash(self):
        self.proc.kill()  # SIGKILL / TerminateProcess: no chance to clean up
        self.proc.wait(10)

    def interrupt(self):
        self.proc.send_signal(signal.SIGINT)
        return self.proc.wait(10)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)


class Conn:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        return raw.decode()

    def cmd(self, text, count=1):
        self.sock.sendall((text + "\n").encode())
        out = []
        while len(out) < count:
            line = self.line()
            if line is None or not line.startswith("210 "):  # skip notifications
                out.append(line)
        return out

    def close(self):
        self.sock.close()


def build_state(port):
    org, guest = Conn(port), Conn(port)
    assert org.cmd("CREATE_EVENT Party1 Andrew 10/31/2026 07:00PM", 2)[0] == "201 Created Event"
    assert org.cmd("CREATE_EVENT Gone Andrew 10/31/2026 07:00PM", 2)[0] == "201 Created Event"
    assert guest.cmd("JOIN Party1 Giovanna", 2)[0] == "200 OK"
    for c, command in [(guest, "RSVP Party1 Yes"), (guest, "CLAIM_ITEM Party1 Chips"),
                       (org, "CLAIM_ITEM Party1 Cake"), (org, "CLAIM_ITEM Party1 Soda"),
                       (org, "RELEASE_ITEM Party1 Soda"), (org, "UPDATE_EVENT Party1 11/01/2026 06:30PM"),
                       (org, "DELETE_EVENT Gone")]:
        reply = c.cmd(command)[0]
        assert reply.startswith("20"), f"{command}: {reply}"
    return org, guest


def observed_state(port):
    """What a new connection sees after rejoining."""
    c = Conn(port)
    join = c.cmd("JOIN Party1 Andrew", 2)
    who = c.cmd("WHO Party1", 4)
    items = c.cmd("ITEMLIST Party1", 5)
    gone = c.cmd("JOIN Gone Andrew")
    c.close()
    return {"join": join[1], "who": who, "items": items, "gone": gone[0].split(":")[0]}


EXPECTED = {
    "join": "Andrew, you are now a guest of Party1 (11/01/2026 06:30PM).",
    "who": ["200 OK", "Andrew (organizer) - Maybe", "Giovanna - Yes", "END"],
    "items": ["200 OK", "Chips - claimed by Giovanna", "Cake - claimed by Andrew", "Soda - unclaimed", "END"],
    "gone": "404 Not Found",
}

RESULTS = []


def test(test_id, description):
    def wrap(fn):
        RESULTS.append((test_id, description, fn))
        return fn
    return wrap


def with_server(fn):
    workdir = tempfile.mkdtemp(prefix="partyon-persist-")
    server = Server(workdir, free_port())
    try:
        fn(server)
    finally:
        server.stop()
        shutil.rmtree(workdir, ignore_errors=True)


@test("PR-01", "Server killed (crash) and restarted: state matches the state before the kill")
def _():
    def run(server):
        server.start()
        org, guest = build_state(server.port)
        before = observed_state(server.port)
        assert before == EXPECTED, f"before kill: {before}"
        server.crash()
        org.close(); guest.close()
        server.start()
        after = observed_state(server.port)
        assert after == before, f"after restart: {after}"
    with_server(run)


@test("PR-02", "Ctrl+C with clients connected: clients see a close, exit 0, state restored")
def _():
    if os.name == "nt":
        print("      (skipped on Windows: no SIGINT for child processes)")
        return

    def run(server):
        server.start()
        org, guest = build_state(server.port)
        idle = Conn(server.port)
        code = server.interrupt()
        assert code == 0, f"exit code {code}"
        for c in (org, guest, idle):
            # Unread notifications may come first; the connection must then close
            while True:
                line = c.line()
                if line is None:
                    break
                assert line.startswith("210 "), f"unexpected line before close: {line}"
        server.start()
        assert observed_state(server.port) == EXPECTED
    with_server(run)


@test("PR-03", "Identities are not saved: after a restart a client must JOIN again")
def _():
    def run(server):
        server.start()
        org, guest = build_state(server.port)
        server.crash()
        server.start()
        c = Conn(server.port)
        assert c.cmd("WHO Party1")[0].startswith("401"), "identity survived a restart"
        assert c.cmd("JOIN Party1 Giovanna", 2)[0] == "200 OK"
        assert c.cmd("CLAIM_ITEM Party1 Chips")[0].startswith("410"), "claim lost"
    with_server(run)


@test("PR-04", "A damaged event file is skipped; the server still starts and serves the rest")
def _():
    def run(server):
        server.start()
        build_state(server.port)
        server.crash()
        with open(os.path.join(server.workdir, "data", "abcd.event"), "w") as f:
            f.write("PARTYON-EVENT 1\ncode Half\n")  # incomplete write
        server.start()
        assert observed_state(server.port) == EXPECTED
    with_server(run)


def main():
    print(f"PartyOn persistence tests using {SERVER}\n")
    if not os.path.exists(SERVER):
        print("server binary not found; run make first")
        return 1
    failures = 0
    for test_id, description, fn in RESULTS:
        try:
            fn()
            print(f"PASS  {test_id:6} {description}")
        except Exception as error:  # noqa: BLE001
            failures += 1
            print(f"FAIL  {test_id:6} {description}\n      {error!r}")
    print(f"\n{len(RESULTS) - failures}/{len(RESULTS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
