"""Tests for flow_generator.noise_trader."""

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
from flow_generator.noise_trader import run as nt_run  # noqa: E402
from market_maker.fair_value import RandomWalkFairValue  # noqa: E402
from market_maker.main import run as mm_run  # noqa: E402
from market_maker.state import PositionState  # noqa: E402
from venue_client import MAGIC, VenueClient  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VENUE_BIN = REPO / "engine/order-matching-engine/build/venue_server"


def _run(coro):
    return asyncio.run(coro)


def _raw_frame(obj: dict) -> bytes:
    payload = json.dumps(obj).encode()
    return struct.pack(">II", MAGIC, len(payload)) + payload


class _FixedRng:
    """Duck-types random.Random's uniform/choice/randint with fixed values,
    so firing is deterministic instead of waiting out real 2-8s randomness."""

    def __init__(self, interval: float = 0.05, side: str = "BUY", qty: int = 3):
        self._interval = interval
        self._side = side
        self._qty = qty

    def uniform(self, a: float, b: float) -> float:
        return self._interval

    def choice(self, seq):
        return self._side

    def randint(self, a: int, b: int) -> int:
        return self._qty


# ── Fake server: price selection + empty-book skip ──────────────────────


def test_prices_off_current_top_and_skips_when_book_empty():
    async def body():
        sent = []

        async def handler(reader, writer):
            writer.write(_raw_frame({"type": "MARKET_DATA", "symbol": "SIM1",
                                      "best_bid": None, "best_ask": None, "last_trade": None}))
            await writer.drain()
            await asyncio.sleep(0.15)  # a few fixed-interval fires will skip here
            writer.write(_raw_frame({"type": "MARKET_DATA", "symbol": "SIM1",
                                      "best_bid": 99.90, "best_ask": 100.10, "last_trade": None}))
            await writer.drain()
            try:
                while True:
                    header = await reader.readexactly(8)
                    _, length = struct.unpack(">II", header)
                    payload = await reader.readexactly(length)
                    sent.append(json.loads(payload))
            except (asyncio.IncompleteReadError, ConnectionError):
                pass

        server = await asyncio.start_server(handler, "127.0.0.1", 0)
        host, port = server.sockets[0].getsockname()[:2]

        fixed_rng = _FixedRng(interval=0.05, side="BUY", qty=2)
        await nt_run(host, port, run_id="fake", rng=fixed_rng, max_fires=1)

        server.close()
        await server.wait_closed()

        new_orders = [m for m in sent if m["type"] == "NEW_ORDER"]
        assert len(new_orders) == 1, sent
        assert new_orders[0]["side"] == "BUY"
        assert new_orders[0]["price"] == 100.10
        assert new_orders[0]["qty"] == 2

    _run(body())


# ── Real venue: produces genuine fills against the market maker ─────────


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
def test_noise_trader_produces_real_fills_against_market_maker():
    port = 9300
    proc = _start_venue(port)
    try:
        async def body():
            mm_state = PositionState()
            mm_fv = RandomWalkFairValue(start=100.00, sigma=0.02, rng=None)
            mm_task = asyncio.create_task(
                mm_run(port=port, run_id="mm", fair_value_source=mm_fv,
                       state=mm_state, tick_interval=0.2))

            await asyncio.sleep(0.3)  # let the market maker post its first quote

            # Every fire is a BUY crossing the market maker's ask, so the
            # market maker's own fill side is always SELL, inventory can
            # only move one direction regardless of exact partial-fill
            # mechanics, which makes this assertion deterministic without
            # needing to predict exact async-scheduling timing.
            fixed_rng = _FixedRng(interval=0.05, side="BUY", qty=3)
            await nt_run(port=port, run_id="nt", rng=fixed_rng, max_fires=6)

            await asyncio.sleep(0.3)  # let any last FILLs land

            mm_task.cancel()
            try:
                await mm_task
            except asyncio.CancelledError:
                pass

            assert mm_state.inventory < 0, mm_state.inventory
            assert mm_state.cash > 0, mm_state.cash

        _run(body())
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
