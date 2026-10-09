#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f scheduler.cpp || ! -f libsim.so || ! -f interfaces.h ]]; then
  echo "Run this from src with scheduler.cpp, libsim.so, and interfaces.h present."
  exit 1
fi

WORKERS="${HET_WORKERS:-4}"
TRAIN_SEEDS="${HET_TRAIN_SEEDS:-0 1 2 3 4 5 6 7}"
VALID_SEEDS="${HET_VALID_SEEDS:-8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31}"
ALL_SEEDS="$TRAIN_SEEDS $VALID_SEEDS"
TMP=.hetero_tmp
rm -rf "$TMP"; mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/seed_main.cpp" <<'CPP'
#include <cstdlib>
#include "interfaces.h"
int main(int argc, char** argv) {
    unsigned seed = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 0;
    GenerateProcesses(seed);
    InitCores();
    ScheduleTimer(Now() + 1000);
    Simulate();
    return 0;
}
CPP

echo "Compiling heterogeneous scheduler harness..."
g++ -std=gnu++17 -O2 -I. -o "$TMP/sim_hetero" scheduler.cpp "$TMP/seed_main.cpp" \
  -L. -lsim '-Wl,-rpath,$ORIGIN/..'
SIM="$TMP/sim_hetero"
export SIM

RAW_HEADER="policy,size_t,age_t,dvfs,preempt_n,preempt_gap,seed,time_h,energy_kwh,edp"
SUM_HEADER="policy,size_t,age_t,dvfs,preempt_n,preempt_gap,nseeds,mean_time_h,mean_energy_kwh,mean_edp,max_edp,min_edp"

run_one() {
  local pol="$1" st="$2" age="$3" dvfs="$4" pn="$5" pg="$6" seed="$7"
  local out line
  out=$(HET_POLICY="$pol" HET_SIZE_T="$st" HET_AGE_T="$age" HET_DVFS="$dvfs" \
        HET_PREEMPT_N="$pn" HET_PREEMPT_GAP="$pg" "$SIM" "$seed")
  line=$(printf '%s\n' "$out" | awk -F, '/^HET_RESULT,/ {print; exit}')
  [[ -n "$line" ]] || { echo "No result for seed=$seed config=$*" >&2; return 1; }
  printf '%s\n' "$line" | awk -F, -v seed="$seed" 'BEGIN{OFS=","}{print $2,$3,$4,$5,$6,$7,seed,$8,$9,$10}'
}
export -f run_one

aggregate() {
  local raw="$1" sum="$2"
  echo "$SUM_HEADER" > "$sum"
  tail -n +2 "$raw" | awk -F, 'BEGIN{OFS=","}
  {
    key=$1 FS $2 FS $3 FS $4 FS $5 FS $6;
    n[key]++; t[key]+=$8; e[key]+=$9; d[key]+=$10;
    if (!(key in mx) || $10>mx[key]) mx[key]=$10;
    if (!(key in mn) || $10<mn[key]) mn[key]=$10;
  }
  END{
    for(k in n) printf "%s,%d,%.9f,%.9f,%.9f,%.9f,%.9f\n",k,n[k],t[k]/n[k],e[k]/n[k],d[k]/n[k],mx[k],mn[k]
  }' | sort -t, -k10,10g >> "$sum"
}

run_grid() {
  local params="$1" seeds="$2" raw="$3" sum="$4" label="$5"
  local jobs="$TMP/jobs.txt"; : > "$jobs"
  while read -r pol st age dvfs pn pg; do
    [[ -z "${pol:-}" ]] && continue
    for seed in $seeds; do echo "$pol $st $age $dvfs $pn $pg $seed" >> "$jobs"; done
  done < "$params"
  echo "$label ($(wc -l < "$jobs" | tr -d ' ') simulations)"
  echo "$RAW_HEADER" > "$raw"
  xargs -P "$WORKERS" -n 7 bash -c 'run_one "$@"' _ < "$jobs" >> "$raw"
  aggregate "$raw" "$sum"
  echo "  best mean EDP: $(sed -n '2p' "$sum" | awk -F, '{printf "%.6f",$10}')"
}

# Phase 1: pure scheduling/placement policies. No preemption, baseline DVFS.
P1="$TMP/phase1.params"; : > "$P1"
# Baseline robust SJF.
echo "0 15000 0 0 0 0" >> "$P1"
# Two physical queues: short-priority, with optional aging of long jobs.
for st in 12000 15000 18000; do
  for age in 0 5000 20000 50000; do
    echo "1 $st $age 0 0 0" >> "$P1"
  done
done
# Heterogeneous placement threshold sweeps.
for st in 10000 12000 14000 16000 18000 20000; do
  echo "2 $st 0 0 0 0" >> "$P1"  # short big / long small
  echo "3 $st 0 0 0 0" >> "$P1"  # short small / long big
done
# Extreme remaining-work assignment, threshold irrelevant.
echo "4 15000 0 0 0 0" >> "$P1"
echo "5 15000 0 0 0 0" >> "$P1"

run_grid "$P1" "$TRAIN_SEEDS" results_hetero_policy_raw.csv results_hetero_policy_summary.csv \
  "Phase 1/4: job-size queues + heterogeneous placement"

# Top 6 policy configurations get class-dependent DVFS profiles.
sed -n '2,7p' results_hetero_policy_summary.csv | \
  awk -F, 'BEGIN{OFS=" "}{print $1,$2,$3}' > "$TMP/top6_base.params"
P2="$TMP/phase2.params"; : > "$P2"
while read -r pol st age; do
  for dvfs in 0 1 2 3 4; do
    echo "$pol $st $age $dvfs 0 0" >> "$P2"
  done
done < "$TMP/top6_base.params"
run_grid "$P2" "$TRAIN_SEEDS" results_hetero_dvfs_raw.csv results_hetero_dvfs_summary.csv \
  "Phase 2/4: process-size-aware DVFS"

# Top 6 after DVFS get selective preemption / effective-quantum tests.
sed -n '2,7p' results_hetero_dvfs_summary.csv | \
  awk -F, 'BEGIN{OFS=" "}{print $1,$2,$3,$4}' > "$TMP/top6_dvfs.params"
P3="$TMP/phase3.params"; : > "$P3"
while read -r pol st age dvfs; do
  echo "$pol $st $age $dvfs 0 0" >> "$P3"
  for pn in 1 4 16; do
    for gap in 2000 5000 8000; do
      echo "$pol $st $age $dvfs $pn $gap" >> "$P3"
    done
  done
done < "$TMP/top6_dvfs.params"
run_grid "$P3" "$TRAIN_SEEDS" results_hetero_preempt_raw.csv results_hetero_preempt_summary.csv \
  "Phase 3/4: selective preemption / effective time quantum"

# Merge all training summaries and validate the best 10 unique settings on unseen seeds.
{
  tail -n +2 results_hetero_policy_summary.csv
  tail -n +2 results_hetero_dvfs_summary.csv
  tail -n +2 results_hetero_preempt_summary.csv
} | sort -t, -k10,10g \
  | awk -F, '!seen[$1 FS $2 FS $3 FS $4 FS $5 FS $6]++' \
  | sed -n '1,10p' \
  | awk -F, 'BEGIN{OFS=" "}{print $1,$2,$3,$4,$5,$6}' > "$TMP/top10.params"

run_grid "$TMP/top10.params" "$VALID_SEEDS" results_hetero_validation_raw.csv results_hetero_validation_summary.csv \
  "Phase 4/4: unseen-seed validation"

run_grid "$TMP/top10.params" "$ALL_SEEDS" results_hetero_allseeds_raw.csv results_hetero_allseeds_summary.csv \
  "Final: all-seed ranking"

echo
echo "============================================================"
echo "BEST JOB-SIZE-AWARE RESULT (seeds 0-31)"
echo "$SUM_HEADER"
sed -n '2p' results_hetero_allseeds_summary.csv
echo "============================================================"
echo "TOP 10"
echo "$SUM_HEADER"
sed -n '2,11p' results_hetero_allseeds_summary.csv
echo
echo "Files to send back:"
echo "  results_hetero_policy_summary.csv"
echo "  results_hetero_dvfs_summary.csv"
echo "  results_hetero_preempt_summary.csv"
echo "  results_hetero_validation_summary.csv"
echo "  results_hetero_allseeds_summary.csv"
