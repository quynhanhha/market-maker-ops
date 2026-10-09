
from __future__ import annotations

import itertools
import os
from datetime import datetime, timezone

SYMBOL = "SIM1"


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def make_client_order_id(prefix: str, run_id: str | None = None):
    """Generate prefix-run_id-sequence IDs; run_id defaults to the process ID."""
    run_id = run_id or str(os.getpid())
    seq = itertools.count(1)

    def next_id() -> str:
        return f"{prefix}-{run_id}-{next(seq)}"

    return next_id
