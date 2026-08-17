"""Fast, synchronous tests for phase2_acceptance_test.py's criterion
functions. construct IntegrationResult by hand, no venue, no async run."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from market_maker.state import PositionState  # noqa: E402
from tools.phase2_acceptance_test import (  # noqa: E402
    IntegrationResult,
    c1_market_maker_quoted,
    c2_flow_generator_produced_real_fills,
    c3_fills_moved_state,
    c4_reservation_price_direction_correct,
    c5_tick_cadence_roughly_1s,
    c6_pnl_logged_every_tick,
    c7_no_crash,
)


def _result(**overrides) -> IntegrationResult:
    defaults = dict(
        state=PositionState(),
        fair_value=100.0,
        tick_lines=["[market_maker] tick fv=100.0000 inv=0.0 cash=0.00 pnl=0.00 bid=99.98 ask=100.02"],
        last_trade=None,
        mm_error=None,
        nt_error=None,
    )
    defaults.update(overrides)
    return IntegrationResult(**defaults)


def test_c1_fails_with_no_ticks():
    ok, _ = c1_market_maker_quoted(_result(tick_lines=[]))
    assert not ok
    ok, _ = c1_market_maker_quoted(_result())
    assert ok


def test_c2_checks_last_trade_presence():
    ok, _ = c2_flow_generator_produced_real_fills(_result(last_trade=None))
    assert not ok
    ok, _ = c2_flow_generator_produced_real_fills(_result(last_trade=100.02))
    assert ok


def test_c3_checks_cash_moved():
    ok, _ = c3_fills_moved_state(_result(state=PositionState(cash=0.0, inventory=0.0)))
    assert not ok
    ok, _ = c3_fills_moved_state(_result(state=PositionState(cash=-999.80, inventory=10)))
    assert ok


def test_c4_direction_correct_for_long_inventory():
    state = PositionState(cash=-1000.0, inventory=10)
    ok, _ = c4_reservation_price_direction_correct(_result(state=state, fair_value=100.0))
    assert ok  # reservation_price(100, 10) = 99.90 < 100.0


def test_c4_direction_correct_for_short_inventory():
    state = PositionState(cash=1000.0, inventory=-10)
    ok, _ = c4_reservation_price_direction_correct(_result(state=state, fair_value=100.0))
    assert ok  # reservation_price(100, -10) = 100.10 > 100.0


def test_c4_inconclusive_on_zero_inventory():
    ok, detail = c4_reservation_price_direction_correct(_result(state=PositionState()))
    assert ok
    assert "inconclusive" in detail


def test_c5_within_tolerance_passes():
    lines = ["x"] * 30
    ok, _ = c5_tick_cadence_roughly_1s(_result(tick_lines=lines), duration=30.0, tick_interval=1.0)
    assert ok


def test_c5_far_outside_tolerance_fails():
    lines = ["x"] * 3
    ok, _ = c5_tick_cadence_roughly_1s(_result(tick_lines=lines), duration=30.0, tick_interval=1.0)
    assert not ok


def test_c6_fails_if_any_tick_line_missing_pnl():
    lines = ["tick pnl=0.20 bid=99.98", "tick bid=99.98 ask=100.02"]  # second missing pnl=
    ok, _ = c6_pnl_logged_every_tick(_result(tick_lines=lines))
    assert not ok


def test_c6_passes_when_all_lines_have_pnl():
    lines = ["tick pnl=0.20", "tick pnl=0.13"]
    ok, _ = c6_pnl_logged_every_tick(_result(tick_lines=lines))
    assert ok


def test_c6_fails_on_empty_ticks():
    ok, _ = c6_pnl_logged_every_tick(_result(tick_lines=[]))
    assert not ok


def test_c7_fails_on_either_error():
    ok, _ = c7_no_crash(_result(mm_error=RuntimeError("boom")))
    assert not ok
    ok, _ = c7_no_crash(_result(nt_error=RuntimeError("boom")))
    assert not ok
    ok, _ = c7_no_crash(_result())
    assert ok
