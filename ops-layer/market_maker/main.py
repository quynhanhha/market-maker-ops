"""Market maker entry point.
"""

from __future__ import annotations

import argparse
import asyncio
import itertools
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from market_maker.fair_value import RandomWalkFairValue  # noqa: E402
from market_maker.quoting import QUOTE_SIZE, Quote, compute_quote  # noqa: E402
from market_maker.state import PositionState  # noqa: E402
from venue_client import VenueClient  # noqa: E402

SYMBOL = "SIM1"


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _new_order(client_order_id: str, side: str, price: float, qty: int) -> dict:
    return {"type": "NEW_ORDER", "client_order_id": client_order_id, "symbol": SYMBOL,
            "side": side, "price": price, "qty": qty, "ts": _now_iso()}


def _cancel(client_order_id: str) -> dict:
    return {"type": "CANCEL", "client_order_id": client_order_id, "ts": _now_iso()}


def make_id_generator(run_id: str):
    """mm-{run_id}-{seq}, seq monotonic from 1 — the client_order_id
    scheme locked in phase2-market-maker-spec.md. Reused as-is by commit
    5's tick loop, same generator called more times."""
    seq = itertools.count(1)

    def next_id() -> str:
        return f"mm-{run_id}-{next(seq)}"

    return next_id


async def connect_and_quote_once(
    host: str = "127.0.0.1",
    port: int = 9200,
    *,
    run_id: str | None = None,
    fair_value_source: RandomWalkFairValue | None = None,
    live_timeout: float = 5.0,
    response_timeout: float = 2.0,
) -> tuple[VenueClient, Quote, dict[str, dict]]:
    """Connect, wait for the first HEARTBEAT/MARKET_DATA, compute one
    quote at zero inventory, submit bid+ask, collect ACK/REJECT for each.

    Returns the open client (caller closes it), the computed Quote, and
    the responses collected so far keyed by client_order_id.
    """
    run_id = run_id or str(os.getpid())
    next_id = make_id_generator(run_id)

    client = await VenueClient.connect(host, port)
    live = asyncio.Event()
    responses: dict[str, dict] = {}

    def _mark_live(_msg: dict) -> None:
        live.set()

    def _record(msg: dict) -> None:
        responses[msg["client_order_id"]] = msg

    client.on("HEARTBEAT", _mark_live)
    client.on("MARKET_DATA", _mark_live)
    client.on("ACK", _record)
    client.on("REJECT", _record)
    client.start()

    await asyncio.wait_for(live.wait(), timeout=live_timeout)

    fv = fair_value_source or RandomWalkFairValue()
    fair_value = fv.next()
    quote = compute_quote(fair_value, inventory=0)

    bid_id = next_id()
    ask_id = next_id()
    await client.send(_new_order(bid_id, "BUY", quote.bid, QUOTE_SIZE))
    await client.send(_new_order(ask_id, "SELL", quote.ask, QUOTE_SIZE))

    order_ids = (bid_id, ask_id)
    deadline = asyncio.get_running_loop().time() + response_timeout
    while not all(oid in responses for oid in order_ids) and asyncio.get_running_loop().time() < deadline:
        await asyncio.sleep(0.02)

    for oid in order_ids:
        r = responses.get(oid)
        if r is None:
            print(f"[market_maker] {oid}: no response")
        elif r["type"] == "ACK":
            print(f"[market_maker] {oid} ACK exchange_order_id={r['exchange_order_id']}")
        else:
            print(f"[market_maker] {oid} REJECT reason={r.get('reason')}")

    return client, quote, responses


async def run(
    host: str = "127.0.0.1",
    port: int = 9200,
    *,
    run_id: str | None = None,
    fair_value_source: RandomWalkFairValue | None = None,
    state: PositionState | None = None,
    tick_interval: float = 1.0,
    live_timeout: float = 5.0,
    max_ticks: int | None = None,
) -> None:
    """Connect, wait for the first HEARTBEAT/MARKET_DATA, then requote
    every tick_interval seconds: step fair value, recompute bid/ask from
    current inventory, cancel the previous resting pair unconditionally,
    submit the new one. Incoming FILLs update PositionState live.

    max_ticks stops the loop after N ticks, None (the default, used by
    main()) runs forever.
    """
    run_id = run_id or str(os.getpid())
    next_id = make_id_generator(run_id)
    fv = fair_value_source or RandomWalkFairValue()
    state = state if state is not None else PositionState()

    client = await VenueClient.connect(host, port)
    live = asyncio.Event()
    order_side: dict[str, str] = {}

    def _mark_live(_msg: dict) -> None:
        live.set()

    def _on_fill(msg: dict) -> None:
        side = order_side.get(msg["client_order_id"])
        if side is None:
            return  # already pruned — older than the one-tick grace period
        state.apply_fill(side, msg["fill_qty"], msg["fill_price"])
        print(f"[market_maker] FILL {msg['client_order_id']} {side} "
              f"{msg['fill_qty']}@{msg['fill_price']} "
              f"inv={state.inventory} cash={state.cash:.2f}")

    def _on_reject(msg: dict) -> None:
        print(f"[market_maker] REJECT {msg.get('client_order_id')} reason={msg.get('reason')}")

    client.on("HEARTBEAT", _mark_live)
    client.on("MARKET_DATA", _mark_live)
    client.on("FILL", _on_fill)
    client.on("REJECT", _on_reject)
    client.start()

    await asyncio.wait_for(live.wait(), timeout=live_timeout)

    resting: list[str] = []
    ticks = 0
    try:
        while max_ticks is None or ticks < max_ticks:
            fair_value = fv.next()
            quote = compute_quote(fair_value, state.inventory)

            for oid in resting:
                await client.send(_cancel(oid))

            # Bound order_side growth: keep only ids that might still get a
            # delayed FILL/CANCEL_REJECT (the pair just cancelled) plus the
            # pair about to be submitted.
            order_side = {oid: order_side[oid] for oid in resting if oid in order_side}

            bid_id, ask_id = next_id(), next_id()
            order_side[bid_id] = "BUY"
            order_side[ask_id] = "SELL"
            await client.send(_new_order(bid_id, "BUY", quote.bid, QUOTE_SIZE))
            await client.send(_new_order(ask_id, "SELL", quote.ask, QUOTE_SIZE))
            resting = [bid_id, ask_id]

            pnl = state.mark_to_market_pnl(fair_value)
            print(f"[market_maker] tick fv={fair_value:.4f} inv={state.inventory} "
                  f"cash={state.cash:.2f} pnl={pnl:.2f} bid={quote.bid} ask={quote.ask}")

            ticks += 1
            await asyncio.sleep(tick_interval)
    finally:
        await client.close()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=int(os.environ.get("VENUE_PORT", 9200)))
    args = ap.parse_args()
    try:
        asyncio.run(run(args.host, args.port))
    except KeyboardInterrupt:
        print("\n[market_maker] stopped")


if __name__ == "__main__":
    main()
