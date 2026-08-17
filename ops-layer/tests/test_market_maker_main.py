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
from market_maker.main import connect_and_quote_once, make_id_generator  # noqa: E402
from venue_client import MAGIC, VenueClient  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VENUE_BIN = REPO / "engine/order-matching-engine/build/venue_server"


def _run(coro):
    return asyncio.run(coro)


def _raw_frame(obj: dict) -> bytes:
    payload = json.dumps(obj).encode()
    return struct.pack(">II", MAGIC, len(payload)) + payload


# ── Pure unit test ────────────────────────────────────────────────────────


def test_id_generator_increments_and_is_independent():
    gen_a = make_id_generator("A")
    gen_b = make_id_generator("B")
    assert gen_a() == "mm-A-1"
    assert gen_a() == "mm-A-2"
    assert gen_b() == "mm-B-1"
    assert gen_a() == "mm-A-3"


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
