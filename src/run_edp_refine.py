#!/usr/bin/env python3
import csv
import os
import re
import subprocess
import sys
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

SRC = Path.cwd()
SIM = SRC / "sim"

HEADER = [
    "small", "big", "small_idle", "big_idle", "small_first",
    "initial_awake", "small_p", "big_p", "policy", "wake_threshold",
    "time_h", "energy_kwh", "edp"
]
RESULT_RE = re.compile(r"^RESULT,(.*)$", re.MULTILINE)


def compile_sim():
    subprocess.run([
        "g++", "-std=gnu++17", "-O2", "-o", "sim", "scheduler.cpp",
        "-L.", "-lsim", "-Wl,-rpath,$ORIGIN"
    ], cwd=SRC, check=True)


def run_one(small, big, small_idle, big_idle, order, initial_awake,
            small_p, big_p, policy, wake):
    env = os.environ.copy()
    env.update({
        "TEST_NUM_SMALL": str(small),
        "TEST_NUM_BIG": str(big),
        "TEST_SMALL_IDLE": str(small_idle),
        "TEST_BIG_IDLE": str(big_idle),
        "TEST_SMALL_FIRST": str(order),
        "TEST_INITIAL_AWAKE": str(initial_awake),
        "TEST_SMALL_P": str(small_p),
        "TEST_BIG_P": str(big_p),
        "TEST_POLICY": str(policy),
        "TEST_WAKE": str(wake),
    })
    p = subprocess.run(
        [str(SIM)], cwd=SRC, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120
    )
    m = RESULT_RE.search(p.stdout)
    if p.returncode != 0 or not m:
        raise RuntimeError(f"Simulation failed for config:\n{p.stdout}")
    f = m.group(1).strip().split(",")
    if len(f) != 13:
        raise RuntimeError(f"Unexpected RESULT line: {m.group(0)}")
    vals = [int(x) for x in f[:10]] + [float(x) for x in f[10:]]
    return dict(zip(HEADER, vals))


def key(r):
    return tuple(r[h] for h in HEADER[:10])


def write_csv(name, rows):
    with open(SRC / name, "w", newline="") as fp:
        w = csv.DictWriter(fp, fieldnames=HEADER)
        w.writeheader()
        w.writerows(sorted(rows, key=lambda r: r["edp"]))


def fmt(r):
    return (
        f"small={r['small']} big={r['big']} "
        f"idleS=C{r['small_idle']} idleB=C{r['big_idle']} "
        f"order={'small-first' if r['small_first'] else 'big-first'} "
        f"awake={r['initial_awake']} Psmall=P{r['small_p']} Pbig=P{r['big_p']} "
        f"policy={r['policy']} wake={r['wake_threshold']} | "
        f"time={r['time_h']:.6f}h energy={r['energy_kwh']:.9f}kWh "
        f"EDP={r['edp']:.9f}"
    )


def dedupe_best(rows):
    d = {}
    for r in rows:
        k = key(r)
        if k not in d or r["edp"] < d[k]["edp"]:
            d[k] = r
    return sorted(d.values(), key=lambda r: r["edp"])


def progress(label, i, total, rows):
    if i % 25 == 0 or i == total:
        b = min(rows, key=lambda r: r["edp"])
        print(f"  {label}: {i}/{total}, best EDP={b['edp']:.6f}", flush=True)


def run_batch(label, params):
    total = len(params)
    rows = []
    workers = int(os.environ.get("EDP_WORKERS", "4"))
    workers = max(1, min(workers, total))
    with ThreadPoolExecutor(max_workers=workers) as ex:
        futures = [ex.submit(run_one, *p) for p in params]
        for i, fut in enumerate(as_completed(futures), 1):
            rows.append(fut.result())
            progress(label, i, total, rows)
    return rows


def main():
    if not (SRC / "scheduler.cpp").exists() or not (SRC / "libsim.so").exists():
        print("Run this from the project src directory.")
        sys.exit(1)

    print("Compiling...", flush=True)
    compile_sim()
    all_rows = []

    # Phase 1: hardware/DVFS neighborhood around the already-observed optimum.
    # Mixed layouts with at least 5 total cores, plus pure 4-small/4-big controls.
    layouts = [(s, b) for s in range(1, 5) for b in range(1, 5) if s + b >= 5]
    layouts += [(4, 0), (0, 4)]
    params = []
    for s, b in layouts:
        orders = (0, 1) if s and b else (1,)
        for order in orders:
            sps = (2, 3, 4) if s else (0,)
            bps = (2, 3, 4) if b else (0,)
            for sp in sps:
                for bp in bps:
                    params.append((s, b, 6, 6, order, 1, sp, bp, 0, 1))

    print(f"Phase 1/4: hardware + P-state refinement ({len(params)} runs)", flush=True)
    phase1 = run_batch("phase 1", params)
    write_csv("results_refine_phase1.csv", phase1)
    all_rows += phase1

    # Phase 2: separate C4/C6 choices for big and small cores on top 12 configs.
    seeds = dedupe_best(phase1)[:12]
    params = []
    seen = set()
    for r in seeds:
        for si in (4, 6):
            for bi in (4, 6):
                p = (r['small'], r['big'], si, bi, r['small_first'], 1,
                     r['small_p'], r['big_p'], 0, 1)
                if p not in seen:
                    seen.add(p)
                    params.append(p)

    print(f"Phase 2/4: separate big/small idle states ({len(params)} runs)", flush=True)
    phase2 = run_batch("phase 2", params)
    write_csv("results_refine_phase2.csv", phase2)
    all_rows += phase2

    # Phase 3: number of candidate cores that start awake.
    seeds = dedupe_best(all_rows)[:6]
    params = []
    seen = set()
    for r in seeds:
        n = r['small'] + r['big']
        for awake in range(1, n + 1):
            p = (r['small'], r['big'], r['small_idle'], r['big_idle'],
                 r['small_first'], awake, r['small_p'], r['big_p'], 0, 1)
            if p not in seen:
                seen.add(p)
                params.append(p)

    print(f"Phase 3/4: initial-awake sweep ({len(params)} runs)", flush=True)
    phase3 = run_batch("phase 3", params)
    write_csv("results_refine_phase3.csv", phase3)
    all_rows += phase3

    # Phase 4: policy x wake threshold on the top 3 configurations.
    seeds = dedupe_best(all_rows)[:3]
    params = []
    seen = set()
    for r in seeds:
        for policy in range(5):
            for wake in range(1, 9):
                p = (r['small'], r['big'], r['small_idle'], r['big_idle'],
                     r['small_first'], r['initial_awake'], r['small_p'],
                     r['big_p'], policy, wake)
                if p not in seen:
                    seen.add(p)
                    params.append(p)

    print(f"Phase 4/4: scheduling policy + wake threshold ({len(params)} runs)", flush=True)
    phase4 = run_batch("phase 4", params)
    write_csv("results_refine_phase4.csv", phase4)
    all_rows += phase4

    ranked = dedupe_best(all_rows)
    best = ranked[0]

    # Final deterministic verification: run the winner twice more.
    p = key(best)
    v1 = run_one(*p)
    v2 = run_one(*p)
    write_csv("results_refine_validation.csv", [v1, v2])

    print("\n============================================================")
    print("BEST RESULT")
    print(fmt(best))
    print("============================================================")
    print("TOP 15")
    for i, r in enumerate(ranked[:15], 1):
        print(f"{i:2d}. {fmt(r)}")
    print("\nValidation repeats:")
    print("  " + fmt(v1))
    print("  " + fmt(v2))
    print("\nCSV files saved as results_refine_phase1.csv ... phase4.csv")


if __name__ == "__main__":
    main()
