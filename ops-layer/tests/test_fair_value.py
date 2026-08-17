import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from market_maker.fair_value import RandomWalkFairValue  # noqa: E402


def test_deterministic_sequence_with_seeded_rng():
    fv = RandomWalkFairValue(start=100.00, sigma=0.02, rng=random.Random(0))
    expected_rng = random.Random(0)
    expected = 100.00
    for _ in range(5):
        expected += expected_rng.gauss(0.0, 0.02)
        assert fv.next() == expected


def test_zero_sigma_holds_at_start():
    fv = RandomWalkFairValue(start=100.00, sigma=0.0, rng=random.Random(1))
    for _ in range(5):
        assert fv.next() == 100.00


def test_step_is_additive_not_reset_to_start():
    fv = RandomWalkFairValue(start=100.00, sigma=0.02, rng=random.Random(2))
    first = fv.next()
    second = fv.next()
    assert fv.value == second
    assert second != first  # walk continues from the new value, not from start
