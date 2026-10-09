"""Private random-walk fair value, independent of venue market data."""

from __future__ import annotations

import random


class RandomWalkFairValue:
    def __init__(self, start: float = 100.00, sigma: float = 0.02,
                 rng: random.Random | None = None):
        self.value = start
        self.sigma = sigma
        self._rng = rng or random.Random()

    def next(self) -> float:
        self.value += self._rng.gauss(0.0, self.sigma)
        return self.value
