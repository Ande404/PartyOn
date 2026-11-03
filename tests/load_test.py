#!/usr/bin/env python3
"""PartyOn load test: data for the scalability analysis.

For each client count it runs two scenarios against a running server:

  shared    every client joins ONE event and claims its own items. Worst case:
            all commands contend for one event lock, and each claim notifies
            every client (N clients x M claims = N*N*M notification lines).
  separate  every client has ITS OWN event. Commands never contend, so this
            shows how far parallel events scale.

and then measures broadcast time: one UPDATE_EVENT in the shared event until
every client has received the EVENT_UPDATED notification.

Usage:
    ./bin/partyon-server &
    python3 tests/load_test.py [host] [port] [--clients 10,25,50,100] [--commands 20]
"""

import argparse
import socket
import statistics
import sys
import threading
import time
import uuid

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("host", nargs="?", default="127.0.0.1")
parser.add_argument("port", nargs="?", type=int, default=5527)
parser.add_argument("--clients", default="10,25,50,100", help="comma-separated client counts")
parser.add_argument("--commands", type=int, default=20, help="claims per client")
args = parser.parse_args()

DATE, TIME = "10/31/2026", "07:00PM"


class Client:
    def __init__(self):
        self.sock = socket.create_connection((args.host, args.port), timeout=30)
        self.buf = b""
        self.notifications = 0
        self.closed = False

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                self.closed = True
                return None
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        return raw.decode()

    def cmd(self, text, count=1):
        """Send one command; return (reply lines, seconds until the reply)."""
        start = time.perf_counter()
        self.sock.sendall((text + "\n").encode())
        first = self.line()
        while first is not None and first.startswith("210 "):
            self.notifications += 1
            first = self.line()
        rest = [self.line() for _ in range(count - 1)]
        return [first] + rest, time.perf_counter() - start

    def wait_for(self, prefix, timeout):
        self.sock.settimeout(timeout)
        try:
            while True:
                line = self.line()
                if line is None:
                    return False
                if line.startswith(prefix):
                    return True
        except socket.timeout:
            return False
        finally:
            self.sock.settimeout(30)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def run_clients(clients, work):
    barrier = threading.Barrier(len(clients))
    latencies, errors = [], []
    lock = threading.Lock()

    def body(i, c):
        mine = []
        try:
            barrier.wait()
            for cmd in work(i):
                reply, seconds = c.cmd(cmd)
                if reply[0] is None or not reply[0].startswith("200"):
                    raise RuntimeError(f"{cmd}: {reply[0]}")
                mine.append(seconds)
        except Exception as error:  # noqa: BLE001
            with lock:
                errors.append(str(error))
        with lock:
            latencies.extend(mine)

    threads = [threading.Thread(target=body, args=(i, c)) for i, c in enumerate(clients)]
    start = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return time.perf_counter() - start, latencies, errors


def percentile(values, p):
    if not values:
        return float("nan")
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(round(p / 100 * (len(ordered) - 1))))]


def scenario(n, shared):
    run = uuid.uuid4().hex[:6]
    clients = [Client() for _ in range(n)]
    try:
        if shared:
            code = f"Load_{run}"
            assert clients[0].cmd(f"CREATE_EVENT {code} Host {DATE} {TIME}", 2)[0][0].startswith("201")
            for i, c in enumerate(clients[1:], 1):
                assert c.cmd(f"JOIN {code} U{i}", 2)[0][0] == "200 OK"
            work = lambda i: [f"CLAIM_ITEM {code} I{i}_{k}" for k in range(args.commands)]  # noqa: E731
        else:
            for i, c in enumerate(clients):
                assert c.cmd(f"CREATE_EVENT Load_{run}_{i} U {DATE} {TIME}", 2)[0][0].startswith("201")
            work = lambda i: [f"CLAIM_ITEM Load_{run}_{i} I{k}" for k in range(args.commands)]  # noqa: E731

        elapsed, latencies, errors = run_clients(clients, work)

        broadcast = None
        if shared and not errors:
            # Let in-flight notifications settle, then time one fan-out
            for c in clients:
                c.sock.settimeout(0.05)
                try:
                    while c.line() is not None:
                        pass
                except socket.timeout:
                    pass
                c.sock.settimeout(30)
            start = time.perf_counter()
            clients[0].sock.sendall(f"UPDATE_EVENT {code} 11/01/2026 06:30PM\n".encode())
            done = [None] * n

            def waiter(i):
                if clients[i].wait_for(f"210 {code} EVENT_UPDATED", 10):
                    done[i] = time.perf_counter() - start

            ws = [threading.Thread(target=waiter, args=(i,)) for i in range(n)]
            for w in ws:
                w.start()
            for w in ws:
                w.join()
            reached = [d for d in done if d is not None]
            broadcast = (max(reached) if reached else float("nan"), len(reached))

        dropped = sum(1 for c in clients if c.closed)
        return {
            "commands": len(latencies),
            "seconds": elapsed,
            "throughput": len(latencies) / elapsed if elapsed else 0,
            "p50": percentile(latencies, 50) * 1000,
            "p95": percentile(latencies, 95) * 1000,
            "max": (max(latencies) if latencies else float("nan")) * 1000,
            "notifications": sum(c.notifications for c in clients),
            "errors": errors,
            "dropped": dropped,
            "broadcast": broadcast,
        }
    finally:
        for c in clients:
            c.close()


def main():
    counts = [int(x) for x in args.clients.split(",")]
    print(f"PartyOn load test against {args.host}:{args.port}, {args.commands} claims per client\n")
    header = f"{'scenario':9} {'clients':>7} {'cmds':>6} {'cmd/s':>8} {'p50 ms':>7} {'p95 ms':>7} " \
             f"{'max ms':>7} {'notifs':>8} {'broadcast ms':>13} {'errors':>6}"
    print(header)
    print("-" * len(header))
    failed = False
    for n in counts:
        for shared in (True, False):
            r = scenario(n, shared)
            if r["broadcast"]:
                ms, reached = r["broadcast"]
                bcast = f"{ms * 1000:7.1f} ({reached}/{n})"
            else:
                bcast = "-"
            print(f"{'shared' if shared else 'separate':9} {n:7d} {r['commands']:6d} {r['throughput']:8.0f} "
                  f"{r['p50']:7.2f} {r['p95']:7.2f} {r['max']:7.1f} {r['notifications']:8d} {bcast:>13} "
                  f"{len(r['errors']) + r['dropped']:6d}")
            for error in r["errors"][:3]:
                print(f"    error: {error}")
            failed = failed or bool(r["errors"])
    print("\nshared = one event (lock contention + N-way fan-out); separate = one event per client")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
