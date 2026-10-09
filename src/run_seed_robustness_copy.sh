#!/usr/bin/env bash
set -euo pipefail

# Robust EDP search across multiple workload seeds.
# Requires the parameterized scheduler.cpp used by run_edp_refine.sh.

if [[ ! -f scheduler.cpp || ! -f libsim.so || ! -f interfaces.h ]]; then
  echo "Run this from the src directory containing scheduler.cpp, libsim.so, and interfaces.h."
  exit 1
fi

WORKERS="${EDP_WORKERS:-4}"
TMP=".edp_seed_tmp"
rm -rf "$TMP"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# The supplied libsim main always uses seed 0. Build a tiny alternate main
# so we can call the public GenerateProcesses(seed) interface directly.
cat > "$TMP/seed_main.cpp" <<'CPP'
#include <cstdlib>
#include "interfaces.h"

int main(int argc, char** argv) {
    unsigned seed = 0;
    if (argc > 1) {
        seed = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
    }
    GenerateProcesses(seed);
    InitCores();
    ScheduleTimer(Now() + 1000);
    Simulate();
    return 0;
}
CPP

echo "Compiling seedable simulator..."
g++ -std=gnu++17 -O2 -I. -o "$TMP/sim_seed" scheduler.cpp "$TMP/seed_main.cpp" \
  -L. -lsim '-Wl,-rpath,$ORIGIN/..'

# Put a copy beside libsim.so so $ORIGIN/.. resolves correctly regardless of cwd.
# The binary stays in TMP; rpath points to the src directory.
SIM="$TMP/sim_seed"

RAW_HEADER="small,big,small_idle,big_idle,small_first,initial_awake,small_p,big_p,policy,wake_threshold,seed,time_h,energy_kwh,edp"
SUM_HEADER="small,big,small_idle,big_idle,small_first,initial_awake,small_p,big_p,policy,wake_threshold,nseeds,mean_time_h,mean_energy_kwh,mean_edp,max_edp,min_edp"

run_one() {
  local small="$1" big="$2" si="$3" bi="$4" order="$5" awake="$6"
  local sp="$7" bp="$8" policy="$9" wake="${10}" seed="${11}"
  local out line

  if ! out=$( \
    TEST_NUM_SMALL="$small" TEST_NUM_BIG="$big" \
    TEST_SMALL_IDLE="$si" TEST_BIG_IDLE="$bi" \
    TEST_SMALL_FIRST="$order" TEST_INITIAL_AWAKE="$awake" \
    TEST_SMALL_P="$sp" TEST_BIG_P="$bp" \
    TEST_POLICY="$policy" TEST_WAKE="$wake" \
    "$SIM" "$seed" 2>&1 ); then
      echo "FAILED config: $small $big $si $bi $order $awake $sp $bp $policy $wake seed=$seed" >&2
      echo "$out" >&2
      return 1
  fi

  line=$(printf '%s\n' "$out" | awk -F, '/^RESULT,/ {print; exit}')
  if [[ -z "$line" ]]; then
    echo "No RESULT line for seed $seed" >&2
    echo "$out" >&2
    return 1
  fi

  # RESULT has config fields followed by time, energy, EDP.
  # Insert seed between config and metrics.
  printf '%s\n' "$line" | awk -F, -v seed="$seed" 'BEGIN{OFS=","} {
    print $2,$3,$4,$5,$6,$7,$8,$9,$10,$11,seed,$12,$13,$14
  }'
}
export -f run_one
export SIM

aggregate() {
  local raw="$1" summary="$2"
  echo "$SUM_HEADER" > "$summary"
  tail -n +2 "$raw" | awk -F, 'BEGIN{OFS=","}
  {
    key=$1 FS $2 FS $3 FS $4 FS $5 FS $6 FS $7 FS $8 FS $9 FS $10
    n[key]++
    st[key]+=$12; se[key]+=$13; sd[key]+=$14
    if (!(key in mx) || $14>mx[key]) mx[key]=$14
    if (!(key in mn) || $14<mn[key]) mn[key]=$14
  }
  END {
    for (k in n) {
      printf "%s,%d,%.9f,%.9f,%.9f,%.9f,%.9f\n", k,n[k],st[k]/n[k],se[k]/n[k],sd[k]/n[k],mx[k],mn[k]
    }
  }' | sort -t, -k14,14g >> "$summary"
}

run_param_grid() {
  local params="$1" seeds="$2" raw="$3" summary="$4" label="$5"
  local jobs="$TMP/jobs.txt"
  : > "$jobs"
  while read -r s b si bi order awake sp bp policy wake; do
    [[ -z "${s:-}" ]] && continue
    for seed in $seeds; do
      echo "$s $b $si $bi $order $awake $sp $bp $policy $wake $seed" >> "$jobs"
    done
  done < "$params"

  local nruns
  nruns=$(wc -l < "$jobs" | tr -d ' ')
  echo "$label ($nruns simulations)"
  echo "$RAW_HEADER" > "$raw"
  xargs -P "$WORKERS" -n 11 bash -c 'run_one "$@"' _ < "$jobs" >> "$raw"
  aggregate "$raw" "$summary"
  echo "  best mean EDP: $(sed -n '2p' "$summary" | awk -F, '{printf "%.6f", $14}')"
}

# ---------------------------------------------------------------------------
# Phase A: P-state robustness around the seed-0 winner.
# ---------------------------------------------------------------------------
P_A="$TMP/p_a.params"
: > "$P_A"
for sp in 2 3 4; do
  for bp in 2 3 4; do
    echo "4 4 6 6 1 4 $sp $bp 0 2" >> "$P_A"
  done
done
run_param_grid "$P_A" "0 1 2 3 4 5 6 7" \
  results_robust_pstates_raw.csv results_robust_pstates_summary.csv \
  "Phase A/3: P-state robustness"

# Use the best P-state pair from Phase A in the next sweep.
BEST_A=$(sed -n '2p' results_robust_pstates_summary.csv)
IFS=, read -r _s _b _si _bi _order _awake BEST_SP BEST_BP _policy _wake _n _mt _me _medp _max _min <<< "$BEST_A"

# ---------------------------------------------------------------------------
# Phase B: policy, wake threshold, and initial-awake robustness.
# Full 8-core candidate pool remains available, but sleeping cores wake only
# as the policy requests them.
# ---------------------------------------------------------------------------
P_B="$TMP/p_b.params"
: > "$P_B"
for awake in 1 2 3 4 5 6 7 8; do
  for policy in 0 1 2 3 4; do
    for wake in 1 2 3 4 5 6 7 8; do
      echo "4 4 6 6 1 $awake $BEST_SP $BEST_BP $policy $wake" >> "$P_B"
    done
  done
done
run_param_grid "$P_B" "0 1 2 3 4 5 6 7" \
  results_robust_policy_raw.csv results_robust_policy_summary.csv \
  "Phase B/3: awake/policy/wake robustness"

# ---------------------------------------------------------------------------
# Phase C: validate the top 10 Phase-B configurations on unseen seeds 8-31.
# ---------------------------------------------------------------------------
P_C="$TMP/p_c.params"
sed -n '2,11p' results_robust_policy_summary.csv | awk -F, 'BEGIN{OFS=" "} {print $1,$2,$3,$4,$5,$6,$7,$8,$9,$10}' > "$P_C"
run_param_grid "$P_C" "8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31" \
  results_robust_validation_raw.csv results_robust_validation_summary.csv \
  "Phase C/3: unseen-seed validation"

# ---------------------------------------------------------------------------
# Final score: combine training seeds 0-7 and validation seeds 8-31 for the
# same top-10 candidates, then rank by mean EDP.
# ---------------------------------------------------------------------------
P_ALL="$TMP/p_all.params"
cp "$P_C" "$P_ALL"
run_param_grid "$P_ALL" "0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31" \
  results_robust_allseeds_raw.csv results_robust_allseeds_summary.csv \
  "Final: all-seed ranking"

echo
echo "============================================================"
echo "BEST ROBUST RESULT (ranked by mean EDP over seeds 0-31)"
echo "$SUM_HEADER"
sed -n '2p' results_robust_allseeds_summary.csv
echo "============================================================"
echo "TOP 10 ROBUST CONFIGURATIONS"
echo "$SUM_HEADER"
sed -n '2,11p' results_robust_allseeds_summary.csv
echo
echo "Saved:"
echo "  results_robust_pstates_summary.csv"
echo "  results_robust_policy_summary.csv"
echo "  results_robust_validation_summary.csv"
echo "  results_robust_allseeds_summary.csv"
