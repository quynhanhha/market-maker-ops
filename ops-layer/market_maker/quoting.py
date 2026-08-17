"""Reservation price and bid/ask quote calculation.
"""

from __future__ import annotations

from typing import NamedTuple

SPREAD = 0.04
SKEW_K = 0.01
QUOTE_SIZE = 10


class Quote(NamedTuple):
    bid: float
    ask: float


def reservation_price(fair_value: float, inventory: float, k: float = SKEW_K) -> float:
    return fair_value - k * inventory


def compute_quote(fair_value: float, inventory: float,
                   spread: float = SPREAD, k: float = SKEW_K) -> Quote:
    """Bid/ask around the inventory-skewed reservation price.

    Rounded to 2 decimal places here, at the wire-facing boundary — the
    venue's price parser rejects anything with more precision as
    bad_price, and fair_value/reservation_price stay unrounded upstream
    since PnL math doesn't need cent-alignment.
    """
    rp = reservation_price(fair_value, inventory, k)
    return Quote(bid=round(rp - spread / 2, 2), ask=round(rp + spread / 2, 2))
