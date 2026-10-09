#!/usr/bin/env python3
"""Grid search idle timeouts by mean raw EDP over generator seeds.

Run on Linux, where the supplied libsim.so and GNU linker are available.
The default grid tests 1..2, 1..3, 1..3, and 1..5 quanta (90 builds).
"""

import argparse
import csv
from decimal import Decimal
import itertools
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src"
TESTS = ROOT / "tests"
RENAMES = (
    "CreateProcess",
    "ExitProcess",
    "TimerInterrupt",
    "CStateTransitionComplete",
    "SimulationComplete",
)


def number_values(raw, minimum):
    """Parse a comma-separated list of integers or inclusive ranges."""
    values = []
    try:
        for item in raw.split(","):
            parts = item.strip().split("-")
            if len(parts) == 1:
                values.append(int(parts[0]))
            elif len(parts) == 2:
                low, high = map(int, parts)
                if low > high:
                    raise ValueError("range starts after its end")
                values.extend(range(low, high + 1))
            else:
                raise ValueError("invalid range")
        if not values or any(value < minimum for value in values):
            raise ValueError(f"values must be at least {minimum}")
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid list {raw!r}: {error}") from error
    return list(dict.fromkeys(values))


def positive_values(raw):
    return number_values(raw, 1)


def seed_values(raw):
    return number_values(raw, 0)


def run(command, *, capture=False):
    try:
        return subprocess.run(command, check=True, text=True,
                              stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                              stderr=subprocess.PIPE).stdout
    except subprocess.CalledProcessError as error:
        print(f"Command failed: {shlex.join(command)}", file=sys.stderr)
        if error.stdout:
            print(error.stdout, file=sys.stderr)
        if error.stderr:
            print(error.stderr, file=sys.stderr)
        raise SystemExit(error.returncode) from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--q2", type=positive_values, default=positive_values("1-2"),
                        help="C1 timeout candidates, e.g. 1-4 or 1,2,4 (default: 1-2)")
    parser.add_argument("--q3", type=positive_values, default=positive_values("1-3"),
                        help="C2 timeout candidates (default: 1-3)")
    parser.add_argument("--q4", type=positive_values, default=positive_values("1-3"),
                        help="C3 timeout candidates (default: 1-3)")
    parser.add_argument("--q6", type=positive_values, default=positive_values("1-5"),
                        help="C4 timeout candidates (default: 1-5)")
    parser.add_argument("--seeds", type=seed_values,
                        default=seed_values("0-5"),
                        help="generator seed numbers (default: 0-5)")
    parser.add_argument("--output", type=Path, default=Path("timeout-grid.csv"),
                        help="ranked CSV destination (default: timeout-grid.csv)")
    args = parser.parse_args()
    seeds = args.seeds
    compiler = shlex.split(os.environ.get("CXX", "g++"))
    if not compiler:
        parser.error("CXX must name a compiler")

    common = compiler + ["-std=gnu++17", "-O2", "-Wall", "-Wextra", "-Werror",
                         f"-I{SOURCE}"]
    grid = list(itertools.product(args.q2, args.q3, args.q4, args.q6))
    rows = []
    with tempfile.TemporaryDirectory(prefix="eec-timeout-grid-") as directory:
        temporary = Path(directory)
        driver = temporary / "driver.o"
        queue = temporary / "ready_queue.o"
        policy = temporary / "policy.o"
        benchmark = temporary / "benchmark"
        run(common + ["-c", str(TESTS / "benchmark_driver.cpp"), "-o", str(driver)])
        run(common + ["-c", str(SOURCE / "ready_queue.cpp"), "-o", str(queue)])

        for index, quanta in enumerate(grid, 1):
            flags = [f"-DEEC_TIMEOUT_QUANTA_{state}={value}"
                     for state, value in zip((2, 3, 4, 6), quanta)]
            rename_flags = [f"-D{name}=Policy{name}" for name in RENAMES]
            run(common + rename_flags + flags + ["-c", str(SOURCE / "scheduler.cpp"),
                                                 "-o", str(policy)])
            run(compiler + [str(driver), str(policy), str(queue), f"-L{SOURCE}",
                            "-lsim", f"-Wl,-rpath,{SOURCE}",
                            "-Wl,--wrap=_Z11LoadContextjj",
                            "-Wl,--wrap=_Z11SaveContextjj", "-o", str(benchmark)])

            row = dict(zip(("q2", "q3", "q4", "q6"), quanta))
            edps = []
            for seed in seeds:
                output = run([str(benchmark), f"seed:{seed}"], capture=True)
                results = [line.removeprefix("RESULT ") for line in output.splitlines()
                           if line.startswith("RESULT ")]
                if len(results) != 1:
                    raise SystemExit(f"Expected one RESULT for {quanta}, seed {seed}")
                result = json.loads(results[0], parse_float=Decimal)
                if result["created"] != result["completed"]:
                    raise SystemExit(f"Incomplete workload for {quanta}, seed {seed}")
                edp = Decimal(result["energy"]) * Decimal(result["time"])
                row[f"seed_{seed}_edp"] = edp
                edps.append(edp)
            row["mean_edp"] = sum(edps) / len(edps)
            rows.append(row)
            print(f"[{index}/{len(grid)}] {quanta}: mean EDP {row['mean_edp']}",
                  file=sys.stderr, flush=True)

    rows.sort(key=lambda row: row["mean_edp"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as stream:
        columns = ["q2", "q3", "q4", "q6", "mean_edp"] + [f"seed_{seed}_edp" for seed in seeds]
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    best = rows[0]
    print(f"Best: q2={best['q2']} q3={best['q3']} q4={best['q4']} "
          f"q6={best['q6']} mean EDP={best['mean_edp']}")
    print(f"Ranked results: {args.output.resolve()}")


if __name__ == "__main__":
    main()
