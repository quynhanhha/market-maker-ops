"""Tests for market_maker.main"""

from __future__ import annotations

import asyncio
import json
import random
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from market_maker.fair_value import RandomWalkFairValue  # noqa: E402
from market_maker.main import connect_and_quote_once, run  # noqa: E402
from market_maker.state import PositionState  # noqa: E402
from venue_client import MAGIC, VenueClient  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VENUE_BIN = REPO / "engine/order-matching-engine/build/venue_server"


def _run(coro):
    return asyncio.run(coro)


def _raw_frame(obj: dict) -> bytes:
    payload = json.dumps(obj).encode()
    return struct.pack(">II", MAGIC, len(payload)) + payload


# ── Ordering test against a fake server ──────────────────────────────────


def test_no_order_sent_before_live_signal():
    async def body():
        received_times: list[float] = []
        sent_market_data_at: list[float] = []

        async def handler(reader, writer):
            async def reader_task():
                try:
                    while True:
                        header = await reader.readexactly(8)
                        _, length = struct.unpack(">II", header)
                        await reader.readexactly(length)
                        received_times.append(time.monotonic())
                except (asyncio.IncompleteReadError, ConnectionError):
                    pass

            rt = asyncio.create_task(reader_task())
            await asyncio.sleep(0.3)
            sent_market_data_at.append(time.monotonic())
            writer.write(_raw_frame({"type": "MARKET_DATA", "symbol": "SIM1",
                                      "best_bid": None, "best_ask": None, "last_trade": None}))
            await writer.drain()
            await asyncio.sleep(0.5)
            rt.cancel()
            writer.close()

        server = await asyncio.start_server(handler, "127.0.0.1", 0)
        host, port = server.sockets[0].getsockname()[:2]

        client, _quote, _responses = await connect_and_quote_once(
            host, port, response_timeout=0.3)
        await client.close()
        server.close()
        await server.wait_closed()

        assert sent_market_data_at, "server never sent its live signal"
        assert len(received_times) >= 2, "expected both NEW_ORDERs to arrive"
        assert all(t >= sent_market_data_at[0] for t in received_times), (
            "a NEW_ORDER arrived before the live signal was sent")

    _run(body())


# ── Requote-loop ordering test against a fake server ─────────


def test_tick_cancels_previous_pair_before_resubmitting():
    async def body():
        received: list[tuple[str, str | None]] = []

        async def handler(reader, writer):
            writer.write(_raw_frame({"type": "MARKET_DATA", "symbol": "SIM1",
                                      "best_bid": None, "best_ask": None, "last_trade": None}))
            await writer.drain()
            try:
                while True:
                    header = await reader.readexactly(8)
                    _, length = struct.unpack(">II", header)
                    payload = await reader.readexactly(length)
                    msg = json.loads(payload)
                    received.append((msg["type"], msg.get("client_order_id")))
            except (asyncio.IncompleteReadError, ConnectionError):
                pass

        server = await asyncio.start_server(handler, "127.0.0.1", 0)
        host, port = server.sockets[0].getsockname()[:2]

        fv = RandomWalkFairValue(start=100.00, sigma=0.0, rng=random.Random(0))
        await run(host, port, run_id="fake", fair_value_source=fv,
                   tick_interval=0.05, max_ticks=3)

        server.close()
        await server.wait_closed()

        assert received == [
            ("NEW_ORDER", "mm-fake-1"), ("NEW_ORDER", "mm-fake-2"),
            ("CANCEL", "mm-fake-1"), ("CANCEL", "mm-fake-2"),
            ("NEW_ORDER", "mm-fake-3"), ("NEW_ORDER", "mm-fake-4"),
            ("CANCEL", "mm-fake-3"), ("CANCEL", "mm-fake-4"),
            ("NEW_ORDER", "mm-fake-5"), ("NEW_ORDER", "mm-fake-6"),
        ]

    _run(body())


# ── Real-venue integration test ──────────────────────────────────────────


def _start_venue(port: int) -> subprocess.Popen:
    proc = subprocess.Popen(
        [str(VENUE_BIN), "--port", str(port), "--heartbeat-ms", "1000"],
        stderr=subprocess.PIPE,
    )
    for _ in range(80):
        if proc.poll() is not None:
            raise RuntimeError("venue exited during startup")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return proc
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("venue did not start listening")


@pytest.mark.skipif(not VENUE_BIN.exists(), reason="venue_server not built")
def test_real_venue_quote_lands_on_book():
    port = 9293
    proc = _start_venue(port)
    try:
        async def body():
            fv = RandomWalkFairValue(start=100.00, sigma=0.02, rng=random.Random(0))
            client, quote, responses = await connect_and_quote_once(
                port=port, run_id="test", fair_value_source=fv, response_timeout=2.0)

            assert len(responses) == 2
            for oid, r in responses.items():
                assert r["type"] == "ACK", f"{oid}: {r}"

            pattern = re.compile(r"^mm-test-(\d+)$")
            seqs = []
            for oid in responses:
                m = pattern.match(oid)
                assert m, oid
                seqs.append(int(m.group(1)))
            assert sorted(seqs) == seqs
            assert len(set(seqs)) == 2

            observer = await VenueClient.connect(port=port)
            md = await observer.wait_for("MARKET_DATA", timeout=2.0)
            assert md is not None
            assert md["best_bid"] == quote.bid
            assert md["best_ask"] == quote.ask

            await client.close()
            await observer.close()

        _run(body())
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


@pytest.mark.skipif(not VENUE_BIN.exists(), reason="venue_server not built")
def test_no_orphaned_resting_orders_and_fill_updates_state():
    port = 9296
    proc = _start_venue(port)
    try:
        async def body():
            # sigma=0 pins fair value (and, at zero inventory, the quoted
            # price) across ticks, so stale un-cancelled orders would be
            # indistinguishable from the current pair unless cancellation
            # is actually working.
            fv = RandomWalkFairValue(start=100.00, sigma=0.0, rng=random.Random(0))
            state = PositionState()
            task = asyncio.create_task(
                run(port=port, run_id="orphan", fair_value_source=fv,
                    state=state, tick_interval=0.2))

            await asyncio.sleep(0.9)  # let several ticks elapse while connected

            observer = await VenueClient.connect(port=port)
            fills = []
            observer.on("FILL", lambda m: fills.append(m))
            observer.start()
            # aggressor sized to exceed a single tick's QUOTE_SIZE (10):
            # if stale orders had piled up instead of being cancelled,
            # this would fully fill instead of leaving a remainder.
            await observer.send({"type": "NEW_ORDER", "client_order_id": "aggr",
                                  "symbol": "SIM1", "side": "SELL", "price": 99.98, "qty": 15})
            await asyncio.sleep(0.3)

            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            await observer.close()

            assert len(fills) == 1, fills
            assert fills[0]["fill_qty"] == 10
            assert fills[0]["remaining_qty"] == 5

            assert state.inventory == 10
            assert state.cash == pytest.approx(-999.80)

        _run(body())
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
