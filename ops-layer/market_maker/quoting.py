
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
    """Round quotes to two decimals to satisfy venue validation."""
    rp = reservation_price(fair_value, inventory, k)
    return Quote(bid=round(rp - spread / 2, 2), ask=round(rp + spread / 2, 2))
