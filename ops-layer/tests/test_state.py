import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from market_maker.state import PositionState  # noqa: E402


def test_default_state_is_flat():
    s = PositionState()
    assert s.cash == 0.0
    assert s.inventory == 0.0


def test_buy_scenario_from_spec():
    s = PositionState()
    s.apply_fill("BUY", 10, 99.98)
    assert s.inventory == 10
    assert s.cash == pytest.approx(-999.80)


def test_sell_is_mirror_of_buy():
    s = PositionState()
    s.apply_fill("SELL", 10, 99.98)
    assert s.inventory == -10
    assert s.cash == pytest.approx(999.80)


def test_sequential_fills_accumulate():
    s = PositionState()
    s.apply_fill("BUY", 10, 99.98)
    s.apply_fill("SELL", 4, 100.02)
    assert s.inventory == 6
    assert s.cash == pytest.approx(-999.80 + 4 * 100.02)


def test_mark_to_market_pnl_after_buy_scenario():
    s = PositionState()
    s.apply_fill("BUY", 10, 99.98)
    assert s.mark_to_market_pnl(fair_value=100.00) == pytest.approx(0.20)
    assert s.mark_to_market_pnl(fair_value=99.98) == pytest.approx(0.0)


def test_unknown_side_raises():
    s = PositionState()
    with pytest.raises(ValueError):
        s.apply_fill("HOLD", 1, 100.0)
