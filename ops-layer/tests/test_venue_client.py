"""Tests for venue_client — framing correctness and message dispatch.
"""

from __future__ import annotations

import asyncio
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from venue_client import MAGIC, FramingError, VenueClient, encode_frame  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VENUE_BIN = REPO / "engine/order-matching-engine/build/venue_server"


def _run(coro):
    return asyncio.run(coro)


def _raw_frame(obj: dict) -> bytes:
    """Hand-built frame, independent of venue_client.encode_frame, so
    decode tests aren't just checking the module against itself."""
    payload = json.dumps(obj).encode()
    return struct.pack(">II", MAGIC, len(payload)) + payload


# ── Framing unit tests ───────────────────────────────────────────────────


def test_encode_frame_roundtrip():
    obj = {"type": "NEW_ORDER", "client_order_id": "c1", "symbol": "SIM1",
           "side": "BUY", "price": 100.02, "qty": 10}
    frame = encode_frame(obj)
    magic, length = struct.unpack(">II", frame[:8])
    assert magic == MAGIC
    payload = frame[8:8 + length]
    assert len(frame) == 8 + length
    assert json.loads(payload) == obj


# ── Fake-server dispatch tests ───────────────────────────────────────────


async def _serve_once(frames_to_send, on_connect=None, expect_recv=0):
    """Start a throwaway asyncio server that sends `frames_to_send` (raw
    bytes) to the first connection, optionally reading `expect_recv`
    frames first. Returns (host, port, close_fn)."""
    received = []

    async def handler(reader, writer):
        for _ in range(expect_recv):
            header = await reader.readexactly(8)
            _, length = struct.unpack(">II", header)
            payload = await reader.readexactly(length)
            received.append(json.loads(payload))
        for chunk in frames_to_send:
            writer.write(chunk)
            await writer.drain()
            await asyncio.sleep(0.01)
        # keep the connection open briefly so the client can read everything
        await asyncio.sleep(0.2)
        writer.close()

    server = await asyncio.start_server(handler, "127.0.0.1", 0)
    host, port = server.sockets[0].getsockname()[:2]

    async def close_fn():
        server.close()
        await server.wait_closed()

    return host, port, close_fn, received


def test_dispatch_by_type_and_wildcard():
    async def body():
        host, port, close_fn, _ = await _serve_once([
            _raw_frame({"type": "ACK", "client_order_id": "c1", "exchange_order_id": "e-1"}),
            _raw_frame({"type": "HEARTBEAT", "seq": 1}),
        ])
        client = await VenueClient.connect(host, port)
        acks = []
        everything = []
        client.on("ACK", lambda m: acks.append(m))
        client.on("*", lambda m: everything.append(m))
        client.start()
        await asyncio.sleep(0.3)
        await client.close()
        await close_fn()
        assert len(acks) == 1 and acks[0]["client_order_id"] == "c1"
        assert [m["type"] for m in everything] == ["ACK", "HEARTBEAT"]

    _run(body())


def test_split_frame_assembles_correctly():
    async def body():
        frame = _raw_frame({"type": "FILL", "client_order_id": "c1",
                             "exchange_order_id": "e-1", "fill_qty": 4,
                             "fill_price": 100.0, "remaining_qty": 6})
        # split the single frame across two writes, mid-payload
        split_at = 10
        host, port, close_fn, _ = await _serve_once([frame[:split_at], frame[split_at:]])
        client = await VenueClient.connect(host, port)
        fills = []
        client.on("FILL", lambda m: fills.append(m))
        client.start()
        await asyncio.sleep(0.3)
        await client.close()
        await close_fn()
        assert len(fills) == 1
        assert fills[0]["fill_qty"] == 4

    _run(body())


def test_null_market_data_fields_parse_to_none():
    async def body():
        raw = b'{"type":"MARKET_DATA","symbol":"SIM1","best_bid":null,"best_ask":null,"last_trade":null}'
        frame = struct.pack(">II", MAGIC, len(raw)) + raw
        host, port, close_fn, _ = await _serve_once([frame])
        client = await VenueClient.connect(host, port)
        seen = []
        client.on("MARKET_DATA", lambda m: seen.append(m))
        client.start()
        await asyncio.sleep(0.3)
        await client.close()
        await close_fn()
        assert seen[0]["best_bid"] is None
        assert seen[0]["best_ask"] is None
        assert seen[0]["last_trade"] is None

    _run(body())


def test_bad_magic_raises_framing_error_and_closes():
    async def body():
        bad = struct.pack(">II", 0xDEADBEEF, 2) + b"{}"
        host, port, close_fn, _ = await _serve_once([bad])
        client = await VenueClient.connect(host, port)
        client.start()
        await asyncio.sleep(0.3)
        await client.close()
        await close_fn()
        assert client.closed
        assert isinstance(client.error, FramingError)

    _run(body())


def test_wait_for_without_explicit_start():
    async def body():
        host, port, close_fn, _ = await _serve_once([
            _raw_frame({"type": "HEARTBEAT", "seq": 1}),
        ])
        client = await VenueClient.connect(host, port)
        msg = await client.wait_for("HEARTBEAT", timeout=1.0)
        await client.close()
        await close_fn()
        assert msg is not None and msg["seq"] == 1

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
def test_real_venue_ack_and_heartbeat():
    port = 9291
    proc = _start_venue(port)
    try:
        async def body():
            client = await VenueClient.connect(port=port)
            client.start()
            md = await client.wait_for("MARKET_DATA", timeout=2.0)
            assert md is not None, "expected an immediate MARKET_DATA snapshot on connect"

            await client.send({"type": "NEW_ORDER", "client_order_id": "t1",
                                "symbol": "SIM1", "side": "BUY", "price": 100.00, "qty": 1})
            ack = await client.wait_for("ACK", timeout=2.0)
            assert ack is not None and ack["client_order_id"] == "t1"

            hb = await client.wait_for("HEARTBEAT", timeout=1.5)
            assert hb is not None

            await client.close()

        _run(body())
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
