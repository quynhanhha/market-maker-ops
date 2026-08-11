#!/usr/bin/env python3
"""Phase 1 end-to-end proof for the exchange venue.

Opens two connections (two participants, since self-match is prevented), rests a
SELL on one and crosses it with a BUY on the other, and asserts both sides get an
ACK and a FILL and both observe the HEARTBEAT / MARKET_DATA feed.

    python3 ops-layer/venue_smoketest.py --port 9001

Exits 0 on success, non-zero (with a reason) on any missing/incorrect message.
Stdlib only — the asyncio market maker arrives in Phase 2.
"""

import argparse
import json
import select
import socket
import sys
import time


class Conn:
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=2.0)
        self.sock.setblocking(False)
        self.buf = b""
        self.msgs: list[dict] = []

    def send(self, obj: dict) -> None:
        self.sock.sendall((json.dumps(obj) + "\n").encode())

    def drain(self) -> None:
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                self.buf += chunk
        except (BlockingIOError, InterruptedError):
            pass
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            line = line.strip()
            if line:
                self.msgs.append(json.loads(line))

    def types(self) -> list[str]:
        return [m.get("type") for m in self.msgs]

    def fills(self) -> list[dict]:
        return [m for m in self.msgs if m.get("type") == "FILL"]

    def acks(self) -> list[dict]:
        return [m for m in self.msgs if m.get("type") == "ACK"]

    def close(self) -> None:
        self.sock.close()


def pump(conns: list[Conn], seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        socks = [c.sock for c in conns]
        ready, _, _ = select.select(socks, [], [], 0.1)
        for c in conns:
            if c.sock in ready:
                c.drain()


def fail(reason: str) -> None:
    print(f"SMOKETEST FAIL: {reason}")
    sys.exit(1)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9001)
    args = ap.parse_args()

    a = Conn(args.host, args.port)  # rests the sell
    b = Conn(args.host, args.port)  # crosses with a buy

    a.send({"type": "NEW_ORDER", "order_id": 1, "side": "SELL", "price": 100, "quantity": 10})
    pump([a, b], 0.3)  # let the resting order settle before crossing
    b.send({"type": "NEW_ORDER", "order_id": 2, "side": "BUY", "price": 100, "quantity": 10})

    # Pump long enough to observe at least one heartbeat tick.
    pump([a, b], 1.5)

    if not a.acks():
        fail("connection A never received an ACK for its resting order")
    if not b.acks():
        fail("connection B never received an ACK for its crossing order")

    a_fills, b_fills = a.fills(), b.fills()
    if not a_fills:
        fail("resting side (A) received no FILL")
    if not b_fills:
        fail("aggressor side (B) received no FILL")
    if a_fills[0].get("price") != 100 or b_fills[0].get("price") != 100:
        fail(f"unexpected fill price: A={a_fills[0]}, B={b_fills[0]}")

    for name, c in (("A", a), ("B", b)):
        if "HEARTBEAT" not in c.types():
            fail(f"connection {name} never observed a HEARTBEAT")
        if "MARKET_DATA" not in c.types():
            fail(f"connection {name} never observed MARKET_DATA")

    a.close()
    b.close()

    print("SMOKETEST PASS")
    print(f"  A: ACK={len(a.acks())} FILL={len(a_fills)} "
          f"HEARTBEAT={a.types().count('HEARTBEAT')} MARKET_DATA={a.types().count('MARKET_DATA')}")
    print(f"  B: ACK={len(b.acks())} FILL={len(b_fills)} "
          f"HEARTBEAT={b.types().count('HEARTBEAT')} MARKET_DATA={b.types().count('MARKET_DATA')}")
    print(f"  fill @ {a_fills[0]['price']} x {a_fills[0]['quantity']}")


if __name__ == "__main__":
    main()
