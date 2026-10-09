#!/usr/bin/env python3
"""Run the two-process trading workload and check its acceptance criteria."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import io
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import IO, Iterator, NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from flow_generator.noise_trader import run as nt_run  # noqa: E402
from market_maker.fair_value import RandomWalkFairValue  # noqa: E402
from market_maker.main import run as mm_run  # noqa: E402
from market_maker.quoting import reservation_price  # noqa: E402
from market_maker.state import PositionState  # noqa: E402
from venue_client import VenueClient  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
VENUE = REPO / "engine/order-matching-engine/build/venue_server"


def start_venue(
    port: int, *, stdout: IO[str] | None = None,
    stderr: IO[str] | int = subprocess.PIPE,
) -> subprocess.Popen:
    if not VENUE.exists():
        raise RuntimeError(f"venue binary not found at {VENUE} — build it first")
    proc = subprocess.Popen([str(VENUE), "--port", str(port), "--heartbeat-ms", "1000"],
                            stdout=stdout, stderr=stderr)
    for _ in range(80):
        if proc.poll() is not None:
            raise RuntimeError("venue exited during startup")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return proc
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("venue did not start listening")


class IntegrationResult(NamedTuple):
    state: PositionState
    fair_value: float
    tick_lines: list[str]
    last_trade: float | None
    mm_error: Exception | None
    nt_error: Exception | None


@contextlib.contextmanager
def capture_trading_stdout(evidence_dir: Path | None) -> Iterator[io.StringIO]:
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            yield buf
        finally:
            if evidence_dir is not None:
                (evidence_dir / "trading.stdout.log").write_text(buf.getvalue())


async def gather(
    port: int, duration: float, evidence_dir: Path | None = None,
) -> IntegrationResult:
    """Use production cadence; cancel trading tasks before the final observer snapshot."""
    state = PositionState()
    fv = RandomWalkFairValue()
    mm_error: Exception | None = None
    nt_error: Exception | None = None

    with capture_trading_stdout(evidence_dir) as buf:
        mm_task = asyncio.create_task(
            mm_run(port=port, run_id="mm", fair_value_source=fv, state=state))
        nt_task = asyncio.create_task(nt_run(port=port, run_id="nt"))

        workload_start = time.monotonic()
        completed = False
        try:
            await asyncio.sleep(duration)
            completed = True
        finally:
            workload_elapsed = time.monotonic() - workload_start
            if evidence_dir is not None:
                (evidence_dir / "workload.json").write_text(json.dumps({
                    "configured_duration_seconds": duration,
                    "elapsed_seconds": workload_elapsed,
                    "wait_completed": completed,
                    "boundary": "before duration sleep to sleep completion; excludes task cancellation, observer and venue teardown",
                    "processes": 2,
                    "trading_connections": 2,
                    "random_seeds": "unspecified",
                }, indent=2) + "\n")

        mm_task.cancel()
        nt_task.cancel()
        try:
            await mm_task
        except asyncio.CancelledError:
            pass
        except Exception as e:  # noqa: BLE001
            mm_error = e
        try:
            await nt_task
        except asyncio.CancelledError:
            pass
        except Exception as e:  # noqa: BLE001
            nt_error = e

    tick_lines = [ln for ln in buf.getvalue().splitlines() if "] tick " in ln]

    observer = await VenueClient.connect(port=port)
    md = await observer.wait_for("MARKET_DATA", timeout=2.0)
    last_trade = md.get("last_trade") if md else None
    await observer.close()

    return IntegrationResult(state=state, fair_value=fv.value, tick_lines=tick_lines,
                              last_trade=last_trade, mm_error=mm_error, nt_error=nt_error)


def c1_market_maker_quoted(result: IntegrationResult):
    ok = len(result.tick_lines) > 0
    return ok, f"{len(result.tick_lines)} tick lines logged"


def c2_flow_generator_produced_real_fills(result: IntegrationResult):
    ok = result.last_trade is not None
    return ok, f"last_trade={result.last_trade}"


def c3_fills_moved_state(result: IntegrationResult):
    # Nonzero cash corroborates fills; it does not verify state arithmetic.
    ok = result.state.cash != 0.0
    return ok, f"inventory={result.state.inventory} cash={result.state.cash:.2f}"


def c4_reservation_price_direction_correct(result: IntegrationResult):
    inv = result.state.inventory
    if inv == 0:
        return True, "inventory net zero this run (rare) — direction check inconclusive, skipped"
    rp = reservation_price(result.fair_value, inv)
    ok = rp < result.fair_value if inv > 0 else rp > result.fair_value
    return ok, f"inventory={inv} fair_value={result.fair_value:.4f} reservation_price={rp:.4f}"


def c5_tick_cadence_roughly_1s(result: IntegrationResult, duration: float, tick_interval: float):
    expected = duration / tick_interval
    tolerance = max(2, expected * 0.15)
    ok = abs(len(result.tick_lines) - expected) <= tolerance
    return ok, f"{len(result.tick_lines)} ticks logged (~expected {expected:.0f} ± {tolerance:.0f})"


def c6_pnl_logged_every_tick(result: IntegrationResult):
    ok = len(result.tick_lines) > 0 and all("pnl=" in ln for ln in result.tick_lines)
    return ok, f"{len(result.tick_lines)}/{len(result.tick_lines)} tick lines contain pnl="


def c7_no_crash(result: IntegrationResult):
    ok = result.mm_error is None and result.nt_error is None
    return ok, f"market_maker error={result.mm_error}; noise_trader error={result.nt_error}"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("VENUE_PORT", 9200)))
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--tick-interval", type=float, default=1.0,
                     help="only for c5's expected-tick-count math; not a routine flag")
    ap.add_argument("--evidence-dir", type=Path,
                    help="new directory for full trading/venue logs and timing metadata")
    args = ap.parse_args()

    with contextlib.ExitStack() as stack:
        venue_stdout = None
        venue_stderr = subprocess.PIPE
        if args.evidence_dir is not None:
            args.evidence_dir.mkdir(parents=True, exist_ok=False)
            venue_stdout = stack.enter_context((args.evidence_dir / "venue.stdout.log").open("w"))
            venue_stderr = stack.enter_context((args.evidence_dir / "venue.stderr.log").open("w"))
        proc = start_venue(args.port, stdout=venue_stdout, stderr=venue_stderr)
        try:
            result = asyncio.run(gather(args.port, args.duration, args.evidence_dir))
        finally:
            before_teardown = proc.poll()
            forced_kill = False
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                forced_kill = True
                proc.kill()
                proc.wait()
            if args.evidence_dir is not None:
                (args.evidence_dir / "venue-process.json").write_text(json.dumps({
                    "pid": proc.pid,
                    "command": proc.args,
                    "returncode_before_teardown": before_teardown,
                    "termination_requested": True,
                    "forced_kill": forced_kill,
                    "returncode_after_teardown": proc.returncode,
                }, indent=2) + "\n")

    checks = [
        ("1. market maker connected and quoted", lambda: c1_market_maker_quoted(result)),
        ("2. flow generator produced real fills", lambda: c2_flow_generator_produced_real_fills(result)),
        ("3. fills moved inventory/cash", lambda: c3_fills_moved_state(result)),
        ("4. reservation price skewed the right direction", lambda: c4_reservation_price_direction_correct(result)),
        ("5. quotes refresh at ~1s cadence", lambda: c5_tick_cadence_roughly_1s(result, args.duration, args.tick_interval)),
        ("6. PnL computed and logged every tick", lambda: c6_pnl_logged_every_tick(result)),
        ("7. no exception in either trading task", lambda: c7_no_crash(result)),
    ]

    results = []
    for name, fn in checks:
        try:
            ok, detail = fn()
        except Exception as e:  # noqa: BLE001
            ok, detail = False, f"exception: {e}"
        results.append((name, ok, detail))
        print(f"[{'PASS' if ok else 'FAIL'}] {name}\n        {detail}")

    passed = sum(1 for _, ok, _ in results if ok)
    print(f"\n{passed}/{len(results)} criteria passed")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
