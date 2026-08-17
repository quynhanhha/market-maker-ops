"""Private fair-value process for the market maker.

Gaussian random walk, independent of the venue's MARKET_DATA — the book
only ever reflects the market maker's own quotes plus the flow
generator's crossings, so deriving fair value from it would be circular.
FILL and MARKET_DATA update bookkeeping (state.py), never this.
"""

from __future__ import annotations

import random


class RandomWalkFairValue:
    def __init__(self, start: float = 100.00, sigma: float = 0.02,
                 rng: random.Random | None = None):
        self.value = start
        self.sigma = sigma
        self._rng = rng or random.Random()

    def next(self) -> float:
        """Advance one tick and return the new fair value."""
        self.value += self._rng.gauss(0.0, self.sigma)
        return self.value
