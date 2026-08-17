"""Flow generator ("noise trader").

Fires on a random interval, crosses whatever's currently resting. No fair
value model, no strategy, exists only to guarantee real fills happen.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from venue_client import SYMBOL, VenueClient, make_client_order_id, now_iso  # noqa: E402


async def run(
    host: str = "127.0.0.1",
    port: int = 9200,
    *,
    run_id: str | None = None,
    min_interval: float = 2.0,
    max_interval: float = 8.0,
    rng: random.Random | None = None,
    live_timeout: float = 5.0,
    max_fires: int | None = None,
) -> None:
    """Connect, wait for the first live signal, then repeatedly: sleep a
    random interval, read the currently-tracked best_bid/best_ask, submit
    one marketable order crossing it (random side, qty 1-5) — or skip and
    retry if that side's price isn't available yet (empty book).

    max_fires counts successful submissions only — a skipped, empty-book
    attempt doesn't consume the budget. None (main()'s default) runs
    forever; tests pass a small number so they can await it directly.
    """
    next_id = make_client_order_id("nt", run_id)
    rng = rng or random.Random()

    client = await VenueClient.connect(host, port)
    live = asyncio.Event()
    top: dict[str, float | None] = {"best_bid": None, "best_ask": None}

    def _mark_live(_msg: dict) -> None:
        live.set()

    def _update_top(msg: dict) -> None:
        top["best_bid"] = msg.get("best_bid")
        top["best_ask"] = msg.get("best_ask")
        live.set()

    client.on("HEARTBEAT", _mark_live)
    client.on("MARKET_DATA", _update_top)
    client.start()

    await asyncio.wait_for(live.wait(), timeout=live_timeout)

    fires = 0
    try:
        while max_fires is None or fires < max_fires:
            await asyncio.sleep(rng.uniform(min_interval, max_interval))

            side = rng.choice(["BUY", "SELL"])
            qty = rng.randint(1, 5)
            price = top["best_ask"] if side == "BUY" else top["best_bid"]
            if price is None:
                print(f"[noise_trader] book empty, skipping fire ({side})")
                continue

            oid = next_id()
            await client.send({"type": "NEW_ORDER", "client_order_id": oid, "symbol": SYMBOL,
                                "side": side, "price": price, "qty": qty, "ts": now_iso()})
            print(f"[noise_trader] {oid} {side} {qty}@{price}")
            fires += 1
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
        print("\n[noise_trader] stopped")


if __name__ == "__main__":
    main()
