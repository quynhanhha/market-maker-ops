"""Protocol-layer constants and helpers shared by every venue_client user.

Mirrors the C++ side's own split (framing.h/.cpp for wire bytes,
protocol.h/.cpp for message schema): client.py owns transport/dispatch,
this module owns the small pieces of message-building every process
needs regardless of its own strategy.
"""

from __future__ import annotations

import itertools
import os
from datetime import datetime, timezone

SYMBOL = "SIM1"


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def make_client_order_id(prefix: str, run_id: str | None = None):
    """{prefix}-{run_id}-{seq}, seq monotonic from 1 per generator
    instance. run_id defaults to the process id so concurrent runs on one
    machine don't collide; pass an explicit run_id (e.g. in tests) to
    override. Each process picks its own prefix — e.g.
    make_client_order_id("mm") vs make_client_order_id("nt") — so ids
    stay distinguishable without either process knowing about the other.
    """
    run_id = run_id or str(os.getpid())
    seq = itertools.count(1)

    def next_id() -> str:
        return f"{prefix}-{run_id}-{next(seq)}"

    return next_id
