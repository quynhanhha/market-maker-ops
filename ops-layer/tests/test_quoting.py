import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from market_maker.quoting import SPREAD, Quote, compute_quote, reservation_price  # noqa: E402

FAIR_VALUE = 100.00


def test_zero_inventory_is_symmetric_around_fair_value():
    rp = reservation_price(FAIR_VALUE, inventory=0)
    assert rp == FAIR_VALUE
    q = compute_quote(FAIR_VALUE, inventory=0)
    assert round(q.ask - q.bid, 10) == SPREAD
    assert round(FAIR_VALUE - q.bid, 10) == round(q.ask - FAIR_VALUE, 10)


def test_long_inventory_skews_reservation_price_down():
    rp = reservation_price(FAIR_VALUE, inventory=50)
    assert rp < FAIR_VALUE
    zero = compute_quote(FAIR_VALUE, inventory=0)
    long = compute_quote(FAIR_VALUE, inventory=50)
    assert long.bid < zero.bid
    assert long.ask < zero.ask


def test_short_inventory_skews_reservation_price_up():
    rp = reservation_price(FAIR_VALUE, inventory=-50)
    assert rp > FAIR_VALUE
    zero = compute_quote(FAIR_VALUE, inventory=0)
    short = compute_quote(FAIR_VALUE, inventory=-50)
    assert short.bid > zero.bid
    assert short.ask > zero.ask


def test_quote_prices_are_wire_valid_two_decimal_places():
    for inventory in (-137, -1, 0, 1, 33, 250):
        q = compute_quote(FAIR_VALUE, inventory)
        assert round(q.bid, 2) == q.bid
        assert round(q.ask, 2) == q.ask


def test_bid_below_ask_across_inventory_range():
    for inventory in range(-500, 501, 25):
        q = compute_quote(FAIR_VALUE, inventory)
        assert q.bid < q.ask


def test_quote_is_named_tuple_with_bid_ask_fields():
    q = compute_quote(FAIR_VALUE, inventory=0)
    assert isinstance(q, Quote)
    assert q.bid == q[0]
    assert q.ask == q[1]
