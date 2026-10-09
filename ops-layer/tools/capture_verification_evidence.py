#!/usr/bin/env python3
"""Capture test and acceptance evidence using CMake, CTest and pytest."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "engine/order-matching-engine/build"


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n")


def inspect(command: list[str]) -> str:
    result = subprocess.run(command, cwd=REPO, capture_output=True, text=True)
    return result.stdout + result.stderr


def run_stage(directory: Path, name: str, command: list[str], cwd: Path) -> dict:
    started = datetime.now(timezone.utc).isoformat()
    start = time.monotonic()
    with (directory / f"{name}.stdout.log").open("w") as stdout:
        with (directory / f"{name}.stderr.log").open("w") as stderr:
            result = subprocess.run(command, cwd=cwd, stdout=stdout, stderr=stderr)
    record = {"command": command, "cwd": str(cwd), "started_utc": started,
              "ended_utc": datetime.now(timezone.utc).isoformat(),
              "elapsed_seconds": time.monotonic() - start, "exit_code": result.returncode}
    write_json(directory / f"{name}.json", record)
    print(f"{name}: exit={result.returncode}, elapsed={record['elapsed_seconds']:.3f}s", flush=True)
    return record


def test_counts(path: Path) -> dict:
    cases = list(ET.parse(path).getroot().iter("testcase"))
    counts = {"passed": 0, "failed": 0, "skipped": 0, "errors": 0, "total": len(cases)}
    for case in cases:
        if case.find("error") is not None:
            counts["errors"] += 1
        elif case.find("failure") is not None:
            counts["failed"] += 1
        elif case.find("skipped") is not None or case.get("status") in {"notrun", "disabled"}:
            counts["skipped"] += 1
        else:
            counts["passed"] += 1
    return counts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=float, default=300.0)
    args = parser.parse_args()
    directory = REPO / "evidence" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ-verification")
    directory.mkdir(parents=True, exist_ok=False)
    print(f"Evidence: {directory}", flush=True)

    metadata = {"git_commit": inspect(["git", "rev-parse", "HEAD"]).strip(),
                "git_status": inspect(["git", "status", "--short"]),
                "platform": platform.platform(), "architecture": platform.machine(),
                "python": sys.version, "python_executable": sys.executable,
                "cmake": inspect(["cmake", "--version"]),
                "compiler": inspect(["c++", "--version"]),
                "pytest": inspect([sys.executable, "-m", "pytest", "--version"]),
                "environment": {key: os.environ.get(key) for key in ("LANG", "LC_ALL", "TZ", "VENUE_PORT")},
                "topology": "Python harness (two trading tasks) + C++ venue: two OS processes",
                "random_seeds": "unspecified; repeat procedure, not exact random sequence"}
    if sys.platform == "darwin":
        metadata["hardware"] = inspect(["sysctl", "-n", "machdep.cpu.brand_string", "hw.memsize"])
        metadata["os_version"] = inspect(["sw_vers"])
    write_json(directory / "environment.json", metadata)
    (directory / "working-tree.patch").write_text(inspect(["git", "diff", "HEAD"]))
    paths = inspect(["git", "ls-files", "--cached", "--others", "--exclude-standard"]).splitlines()
    paths = [path for path in paths if (REPO / path).is_file()]
    write_json(directory / "source-sha256.json", {
        path: hashlib.sha256((REPO / path).read_bytes()).hexdigest()
        for path in paths if (REPO / path).is_file()
    })
    # Renamed and uncommitted files cannot be reconstructed from Git HEAD alone.
    for name in paths:
        target = directory / "source" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / name, target)

    stages = {}
    stages["configure"] = run_stage(directory, "configure", [
        "cmake", "-S", str(REPO / "engine/order-matching-engine"), "-B", str(BUILD),
        "-DCMAKE_BUILD_TYPE=", "-DBUILD_TESTS=ON", "-DBUILD_BENCHMARKS=OFF",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"], REPO)
    if stages["configure"]["exit_code"] == 0:
        stages["build"] = run_stage(directory, "build", ["cmake", "--build", str(BUILD), "--parallel", "2"], REPO)
    built = "build" in stages and stages["build"]["exit_code"] == 0
    if built:
        for name, source in (("CMakeCache.txt", BUILD / "CMakeCache.txt"),
                             ("compile_commands.json", BUILD / "compile_commands.json")):
            shutil.copy2(source, directory / name)
        venue = BUILD / "venue_server"
        write_json(directory / "venue-binary.json", {
            "path": str(venue), "sha256": hashlib.sha256(venue.read_bytes()).hexdigest()})
        stages["cpp-tests"] = run_stage(directory, "cpp-tests", [
            "ctest", "--test-dir", str(BUILD), "-V", "--output-on-failure",
            "--output-junit", str(directory / "cpp-tests.xml")], REPO)
        shutil.copy2(BUILD / "Testing/Temporary/LastTest.log", directory / "CTest-LastTest.log")
    stages["python-tests"] = run_stage(directory, "python-tests", [
        sys.executable, "-m", "pytest", "-v", "-rA",
        f"--junitxml={directory / 'python-tests.xml'}"], REPO / "ops-layer")
    if built:
        stages["acceptance"] = run_stage(directory, "acceptance", [
            sys.executable, "ops-layer/tools/trading_acceptance_test.py",
            "--duration", str(args.duration), "--evidence-dir", str(directory / "acceptance")], REPO)

    summary = {"stages": stages, "tests": {}, "scope": "verification"}
    for name in ("cpp-tests", "python-tests"):
        path = directory / f"{name}.xml"
        summary["tests"][name] = test_counts(path) if path.exists() else {"incomplete": True}
    workload_path = directory / "acceptance/workload.json"
    process_path = directory / "acceptance/venue-process.json"
    summary["workload"] = json.loads(workload_path.read_text()) if workload_path.exists() else None
    summary["venue_process"] = json.loads(process_path.read_text()) if process_path.exists() else None
    counts_ok = all(c.get("total", 0) > 0 and not any(c.get(k, 0) for k in ("failed", "skipped", "errors"))
                    for c in summary["tests"].values())
    workload = summary["workload"] or {}
    process = summary["venue_process"] or {}
    summary["successful"] = (built and all(s["exit_code"] == 0 for s in stages.values()) and counts_ok
                             and workload.get("wait_completed", False)
                             and workload.get("elapsed_seconds", 0) >= args.duration
                             and process.get("returncode_before_teardown", 1) is None
                             and not process.get("forced_kill", True))
    write_json(directory / "summary.json", summary)
    write_json(directory / "artifact-sha256.json", {
        str(path.relative_to(directory)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in directory.rglob("*") if path.is_file()
    })
    print(json.dumps(summary, indent=2), flush=True)
    sys.exit(0 if summary["successful"] else 1)


if __name__ == "__main__":
    main()
