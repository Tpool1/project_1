#!/usr/bin/env bash
set -euo pipefail

# Final local refinement around the best heterogeneous scheduler found so far.
# Requires scheduler.cpp to be the heterogeneous harness.

for f in scheduler.cpp libsim.so interfaces.h; do
  if [[ ! -f "$f" ]]; then
    echo "Missing required file: $f"
    echo "Run this script from the project's src directory."
    exit 1
  fi
done

WORKERS="${HET_REFINE_WORKERS:-4}"
TRAIN_SEEDS="${HET_REFINE_TRAIN_SEEDS:-0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15}"
VALID_SEEDS="${HET_REFINE_VALID_SEEDS:-16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31}"
HOLDOUT_SEEDS="${HET_REFINE_HOLDOUT_SEEDS:-32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63}"

TMP=.hetero_refine_tmp
rm -rf "$TMP"
mkdir -p "$TMP"
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
g++ -std=gnu++17 -O2 -I. -o "$TMP/sim_hetero_refine" scheduler.cpp "$TMP/seed_main.cpp" \
  -L. -lsim '-Wl,-rpath,$ORIGIN/..'
SIM="$TMP/sim_hetero_refine"
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
    for(k in n)
      printf "%s,%d,%.9f,%.9f,%.9f,%.9f,%.9f\n",k,n[k],t[k]/n[k],e[k]/n[k],d[k]/n[k],mx[k],mn[k]
  }' | sort -t, -k10,10g >> "$sum"
}

run_grid() {
  local params="$1" seeds="$2" raw="$3" sum="$4" label="$5"
  local jobs="$TMP/jobs.txt"
  : > "$jobs"
  while read -r pol st age dvfs pn pg; do
    [[ -z "${pol:-}" ]] && continue
    for seed in $seeds; do
      echo "$pol $st $age $dvfs $pn $pg $seed" >> "$jobs"
    done
  done < "$params"

  echo "$label ($(wc -l < "$jobs" | tr -d ' ') simulations)"
  echo "$RAW_HEADER" > "$raw"
  xargs -P "$WORKERS" -n 7 bash -c 'run_one "$@"' _ < "$jobs" >> "$raw"
  aggregate "$raw" "$sum"
  echo "  best mean EDP: $(sed -n '2p' "$sum" | awk -F, '{printf "%.6f",$10}')"
}

# ------------------------------------------------------------
# Phase 1: complete local grid around the current winner.
# Fixed: policy 2 (short->big, long->small), age 0, baseline DVFS.
# Sweep size threshold, preemption frequency, and preemption gap.
# Include non-preemptive controls for every size threshold.
# ------------------------------------------------------------
P1="$TMP/local_grid.params"
: > "$P1"
for st in 9000 10000 11000 12000 13000 14000 15000; do
  echo "2 $st 0 0 0 0" >> "$P1"
  for pn in 1 2 4; do
    for pg in 3000 4000 5000 6000 7000; do
      echo "2 $st 0 0 $pn $pg" >> "$P1"
    done
  done
done

run_grid "$P1" "$TRAIN_SEEDS" \
  results_hetero_refine_train_raw.csv results_hetero_refine_train_summary.csv \
  "Phase 1/3: local threshold + preemption search"

# Top 12 on seeds 0-15 get a separate validation on seeds 16-31.
sed -n '2,13p' results_hetero_refine_train_summary.csv | \
  awk -F, 'BEGIN{OFS=" "}{print $1,$2,$3,$4,$5,$6}' > "$TMP/top12.params"

run_grid "$TMP/top12.params" "$VALID_SEEDS" \
  results_hetero_refine_validation_raw.csv results_hetero_refine_validation_summary.csv \
  "Phase 2/3: validation on seeds 16-31"

# Combine training + validation records only for the top 12, yielding an honest
# 0-31 ranking without rerunning simulations.
echo "$RAW_HEADER" > results_hetero_refine_0_31_raw.csv
while read -r pol st age dvfs pn pg; do
  awk -F, -v p="$pol" -v s="$st" -v a="$age" -v d="$dvfs" -v n="$pn" -v g="$pg" \
    'NR>1 && $1==p && $2==s && $3==a && $4==d && $5==n && $6==g {print}' \
    results_hetero_refine_train_raw.csv >> results_hetero_refine_0_31_raw.csv
  awk -F, -v p="$pol" -v s="$st" -v a="$age" -v d="$dvfs" -v n="$pn" -v g="$pg" \
    'NR>1 && $1==p && $2==s && $3==a && $4==d && $5==n && $6==g {print}' \
    results_hetero_refine_validation_raw.csv >> results_hetero_refine_0_31_raw.csv
done < "$TMP/top12.params"
aggregate results_hetero_refine_0_31_raw.csv results_hetero_refine_0_31_summary.csv

echo "  best combined EDP over seeds 0-31: $(sed -n '2p' results_hetero_refine_0_31_summary.csv | awk -F, '{printf "%.6f",$10}')"

# Top 6 selected WITHOUT using the new seeds.
sed -n '2,7p' results_hetero_refine_0_31_summary.csv | \
  awk -F, 'BEGIN{OFS=" "}{print $1,$2,$3,$4,$5,$6}' > "$TMP/top6.params"

# Truly new seeds 32-63 are kept out of tuning and used only as the final check.
run_grid "$TMP/top6.params" "$HOLDOUT_SEEDS" \
  results_hetero_refine_holdout_raw.csv results_hetero_refine_holdout_summary.csv \
  "Phase 3/3: untouched-seed check (32-63)"

# Post-hoc 0-63 statistics for those same six finalists.
echo "$RAW_HEADER" > results_hetero_refine_all64_raw.csv
while read -r pol st age dvfs pn pg; do
  awk -F, -v p="$pol" -v s="$st" -v a="$age" -v d="$dvfs" -v n="$pn" -v g="$pg" \
    'NR>1 && $1==p && $2==s && $3==a && $4==d && $5==n && $6==g {print}' \
    results_hetero_refine_0_31_raw.csv >> results_hetero_refine_all64_raw.csv
  awk -F, -v p="$pol" -v s="$st" -v a="$age" -v d="$dvfs" -v n="$pn" -v g="$pg" \
    'NR>1 && $1==p && $2==s && $3==a && $4==d && $5==n && $6==g {print}' \
    results_hetero_refine_holdout_raw.csv >> results_hetero_refine_all64_raw.csv
done < "$TMP/top6.params"
aggregate results_hetero_refine_all64_raw.csv results_hetero_refine_all64_summary.csv

# Report the config selected from seeds 0-31 and its untouched-seed behavior.
BEST031=$(sed -n '2p' results_hetero_refine_0_31_summary.csv)
IFS=, read -r BP BS BA BD BN BG _ <<< "$BEST031"
BEST_HOLDOUT=$(awk -F, -v p="$BP" -v s="$BS" -v a="$BA" -v d="$BD" -v n="$BN" -v g="$BG" \
  'NR>1 && $1==p && $2==s && $3==a && $4==d && $5==n && $6==g {print; exit}' \
  results_hetero_refine_holdout_summary.csv)
BEST_ALL64=$(awk -F, -v p="$BP" -v s="$BS" -v a="$BA" -v d="$BD" -v n="$BN" -v g="$BG" \
  'NR>1 && $1==p && $2==s && $3==a && $4==d && $5==n && $6==g {print; exit}' \
  results_hetero_refine_all64_summary.csv)

echo
echo "============================================================"
echo "SELECTED CONFIG (chosen only from seeds 0-31)"
echo "$SUM_HEADER"
echo "$BEST031"
echo "============================================================"
echo "SAME CONFIG ON UNTOUCHED SEEDS 32-63"
echo "$SUM_HEADER"
echo "$BEST_HOLDOUT"
echo "============================================================"
echo "SAME CONFIG OVER ALL 64 SEEDS (post-hoc)"
echo "$SUM_HEADER"
echo "$BEST_ALL64"
echo "============================================================"
echo "TOP 6 FINALISTS ON UNTOUCHED SEEDS"
echo "$SUM_HEADER"
sed -n '2,7p' results_hetero_refine_holdout_summary.csv

echo
echo "Files to send back:"
echo "  results_hetero_refine_train_summary.csv"
echo "  results_hetero_refine_0_31_summary.csv"
echo "  results_hetero_refine_holdout_summary.csv"
echo "  results_hetero_refine_all64_summary.csv"
