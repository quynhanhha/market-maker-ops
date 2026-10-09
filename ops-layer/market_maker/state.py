"""FILL messages omit side; callers must supply the submitted order side."""

from __future__ import annotations


class PositionState:
    def __init__(self, cash: float = 0.0, inventory: float = 0.0):
        self.cash = cash
        self.inventory = inventory

    def apply_fill(self, side: str, fill_qty: float, fill_price: float) -> None:
        if side == "BUY":
            self.cash -= fill_qty * fill_price
            self.inventory += fill_qty
        elif side == "SELL":
            self.cash += fill_qty * fill_price
            self.inventory -= fill_qty
        else:
            raise ValueError(f"unknown side: {side!r}")

    def mark_to_market_pnl(self, fair_value: float) -> float:
        return self.cash + self.inventory * fair_value
