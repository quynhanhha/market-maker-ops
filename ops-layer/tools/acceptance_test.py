#!/usr/bin/env python3
"""Phase 1 acceptance test

Spins up the venue process, runs every criterion from phase-1-spec.md's
"Acceptance criteria" against it, tears it down, and exits non-zero on any
failure. Run from the repo root:

    python3 ops-layer/tools/acceptance_test.py [--port 9200]
"""

import argparse
import socket
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_client import MAGIC, VenueClient  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VENUE = REPO / "engine/order-matching-engine/build/venue_server"


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def new_order(cid, side, price, qty, symbol="SIM1") -> dict:
    return {"type": "NEW_ORDER", "client_order_id": cid, "symbol": symbol,
            "side": side, "price": price, "qty": qty, "ts": now_iso()}


def cancel(cid) -> dict:
    return {"type": "CANCEL", "client_order_id": cid, "ts": now_iso()}


def start_venue(port: int) -> subprocess.Popen:
    if not VENUE.exists():
        raise RuntimeError(f"venue binary not found at {VENUE} — build it first")
    proc = subprocess.Popen([str(VENUE), "--port", str(port), "--heartbeat-ms", "1000"],
                            stderr=subprocess.PIPE)
    for _ in range(80):
        if proc.poll() is not None:
            raise RuntimeError("venue exited during startup")
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            s.close()
            return proc
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("venue did not start listening")


# ── Criteria ────────────────────────────────────────────────────────────────
# Each returns (ok: bool, detail: str).

def c1_starts_and_listens(port, proc):
    ok = proc.poll() is None
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
    except OSError as e:
        return False, f"not connectable: {e}"
    return ok, f"running (pid {proc.pid}), listening on {port}"


def c2_new_order_acked(port):
    c = VenueClient(port=port)
    c.send(new_order("c2", "BUY", 100.02, 100))
    ack = c.wait_for("ACK")
    c.close()
    if not ack or ack.get("client_order_id") != "c2":
        return False, f"no ACK (got {c.types()})"
    return True, f"ACK exchange_order_id={ack.get('exchange_order_id')}"


def c3_fill_fields_correct(port):
    a = VenueClient(port=port)  # resting
    b = VenueClient(port=port)  # aggressor
    a.send(new_order("rest", "SELL", 100.00, 10))
    a.wait_for("ACK")
    b.send(new_order("aggr", "BUY", 100.00, 4))
    bf = b.wait_for("FILL")
    a.pump(0.5)
    af = next((m for m in a.of_type("FILL")), None)
    a.close(); b.close()
    if not bf or not af:
        return False, f"missing FILL (a={a.types()}, b={b.types()})"
    if (bf["fill_qty"], bf["fill_price"], bf["remaining_qty"]) != (4, 100.00, 0):
        return False, f"aggressor fill wrong: {bf}"
    if (af["fill_qty"], af["fill_price"], af["remaining_qty"]) != (4, 100.00, 6):
        return False, f"resting fill wrong: {af}"
    return True, f"aggr {bf['fill_qty']}@{bf['fill_price']} rem0; rest rem6"


def c4_heartbeat_cadence(port):
    # Record each heartbeat's arrival time (send nothing) and check the gaps
    # between consecutive heartbeats are ~1s, not just the count.
    c = VenueClient(port=port)
    arrivals = []
    seen = 0
    deadline = time.monotonic() + 4.0
    while time.monotonic() < deadline and len(arrivals) < 4:
        c.pump(0.02)
        hbs = c.of_type("HEARTBEAT")
        while seen < len(hbs):
            arrivals.append(time.monotonic())
            seen += 1
    c.close()

    seqs = [h.get("seq") for h in c.of_type("HEARTBEAT")]
    if len(arrivals) < 3:
        return False, f"only {len(arrivals)} heartbeats in 4s (need ≥3 for 2 gaps)"
    gaps = [arrivals[i + 1] - arrivals[i] for i in range(len(arrivals) - 1)]
    out_of_band = [round(g, 3) for g in gaps if not (0.85 <= g <= 1.2)]
    if out_of_band:
        return False, f"inter-arrival gaps off ~1s: all={[round(g, 3) for g in gaps]}"
    if seqs != sorted(seqs) or len(set(seqs)) != len(seqs):
        return False, f"seq not strictly increasing: {seqs}"
    return True, f"gaps={[round(g, 3) for g in gaps]}s, seq {seqs[0]}..{seqs[-1]}"


def c5_cancel_semantics(port):
    # (a) cancel a resting order → CANCEL_ACK
    c = VenueClient(port=port)
    c.send(new_order("r1", "SELL", 100.00, 10))
    c.wait_for("ACK")
    c.send(cancel("r1"))
    ca = c.wait_for("CANCEL_ACK")
    c.close()
    if not ca:
        return False, f"resting cancel: no CANCEL_ACK ({c.types()})"
    # (b) cancel an already-filled order → CANCEL_REJECT already_filled
    a = VenueClient(port=port)
    b = VenueClient(port=port)
    a.send(new_order("f1", "SELL", 100.00, 5))
    a.wait_for("ACK")
    b.send(new_order("x1", "BUY", 100.00, 5))  # fully fills f1
    b.wait_for("FILL")
    a.pump(0.3)
    a.send(cancel("f1"))
    cr = a.wait_for("CANCEL_REJECT")
    a.close(); b.close()
    if not cr or cr.get("reason") != "already_filled":
        return False, f"already-filled cancel: expected already_filled, got {cr}"
    return True, "CANCEL_ACK on resting; CANCEL_REJECT/already_filled on filled"


def c6_malformed_rejects_no_crash(port, proc):
    c = VenueClient(port=port)
    c.send(new_order("bq", "BUY", 100.00, 0))            # bad_qty
    c.send(new_order("us", "BUY", 100.00, 5, "NOPE"))    # unknown_symbol
    # malformed JSON inside a well-formed frame → REJECT/malformed (not a crash)
    bad = b'{"type":"NEW_ORDER"'
    c.send_raw(struct.pack(">II", MAGIC, len(bad)) + bad)
    c.pump(0.5)
    reasons = {m.get("reason") for m in c.of_type("REJECT")}
    # server must still be alive and serving
    alive = proc.poll() is None
    ok_ack = None
    if alive:
        c.send(new_order("after", "BUY", 100.00, 1))
        ok_ack = c.wait_for("ACK")
    c.close()
    if not {"bad_qty", "unknown_symbol"} <= reasons:
        return False, f"missing REJECT reasons, got {reasons}"
    if not alive:
        return False, "venue crashed on malformed input"
    if not ok_ack:
        return False, "venue unresponsive after malformed input"
    return True, f"rejects={sorted(reasons)}; server survived + still ACKs"


def c7_connection_kill_survives(port, proc):
    victim = VenueClient(port=port)
    victim.send(new_order("k1", "SELL", 100.00, 10))
    victim.wait_for("ACK")
    victim.hard_reset_close()  # abrupt RST mid-session
    time.sleep(0.3)
    if proc.poll() is not None:
        return False, "venue crashed after connection kill"
    fresh = VenueClient(port=port)  # server must still accept new connections
    fresh.send(new_order("k2", "BUY", 100.00, 1))
    ack = fresh.wait_for("ACK")
    fresh.close()
    if not ack:
        return False, "venue did not serve a fresh connection after a kill"
    return True, "survived RST; served a new connection"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9200)
    args = ap.parse_args()

    proc = start_venue(args.port)
    checks = [
        ("1. starts standalone, listens on port", lambda: c1_starts_and_listens(args.port, proc)),
        ("2. NEW_ORDER -> ACK", lambda: c2_new_order_acked(args.port)),
        ("3. FILL with correct qty/price/remaining", lambda: c3_fill_fields_correct(args.port)),
        ("4. HEARTBEAT at ~1s, no input", lambda: c4_heartbeat_cadence(args.port)),
        ("5. CANCEL_ACK / CANCEL_REJECT semantics", lambda: c5_cancel_semantics(args.port)),
        ("6. malformed -> REJECT, no crash", lambda: c6_malformed_rejects_no_crash(args.port, proc)),
        ("7. connection kill doesn't crash venue", lambda: c7_connection_kill_survives(args.port, proc)),
    ]

    results = []
    try:
        for name, fn in checks:
            try:
                ok, detail = fn()
            except Exception as e:  # noqa: BLE001
                ok, detail = False, f"exception: {e}"
            results.append((name, ok, detail))
            print(f"[{'PASS' if ok else 'FAIL'}] {name}\n        {detail}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{len(results)} criteria passed")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
