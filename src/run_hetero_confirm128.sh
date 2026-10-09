#!/usr/bin/env bash
set -euo pipefail

# Final confirmation: 2 finalist configurations x 64 fresh seeds = 128 simulations.
# Requires scheduler.cpp to still be the heterogeneous scheduler harness.

for f in scheduler.cpp libsim.so interfaces.h; do
  if [[ ! -f "$f" ]]; then
    echo "Missing required file: $f"
    echo "Run this script from the project's src directory."
    exit 1
  fi
done

WORKERS="${HET_CONFIRM_WORKERS:-4}"
SEEDS=$(seq 64 127)

TMP=.hetero_confirm128_tmp
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

echo "Compiling..."
g++ -std=gnu++17 -O2 -I. -o "$TMP/sim_confirm" scheduler.cpp "$TMP/seed_main.cpp" \
  -L. -lsim '-Wl,-rpath,$ORIGIN/..'

SIM="$TMP/sim_confirm"
export SIM

RAW="results_hetero_confirm128_raw.csv"
SUM="results_hetero_confirm128_summary.csv"

echo "name,policy,size_t,age_t,dvfs,preempt_n,preempt_gap,seed,time_h,energy_kwh,edp" > "$RAW"

run_one() {
    local name="$1" pol="$2" st="$3" age="$4" dvfs="$5" pn="$6" pg="$7" seed="$8"
    local out line
    out=$(HET_POLICY="$pol" HET_SIZE_T="$st" HET_AGE_T="$age" HET_DVFS="$dvfs" \
          HET_PREEMPT_N="$pn" HET_PREEMPT_GAP="$pg" "$SIM" "$seed")
    line=$(printf '%s\n' "$out" | awk -F, '/^HET_RESULT,/ {print; exit}')
    [[ -n "$line" ]] || { echo "No result: $name seed=$seed" >&2; return 1; }

    printf '%s\n' "$line" | awk -F, -v name="$name" -v seed="$seed" \
      'BEGIN{OFS=","}{print name,$2,$3,$4,$5,$6,$7,seed,$8,$9,$10}'
}
export -f run_one

JOBS="$TMP/jobs.txt"
: > "$JOBS"

for seed in $SEEDS; do
    echo "A_12000_p1_g5000 2 12000 0 0 1 5000 $seed" >> "$JOBS"
done

for seed in $SEEDS; do
    echo "B_10000_p4_g3000 2 10000 0 0 4 3000 $seed" >> "$JOBS"
done

COUNT=$(wc -l < "$JOBS" | tr -d ' ')
echo "Running $COUNT simulations on fresh seeds 64-127..."

xargs -P "$WORKERS" -n 8 bash -c 'run_one "$@"' _ < "$JOBS" >> "$RAW"

echo "name,nseeds,mean_time_h,mean_energy_kwh,mean_edp,max_edp,min_edp" > "$SUM"

tail -n +2 "$RAW" | awk -F, '
BEGIN{OFS=","}
{
    n[$1]++
    t[$1]+=$9
    e[$1]+=$10
    d[$1]+=$11
    if (!($1 in mx) || $11>mx[$1]) mx[$1]=$11
    if (!($1 in mn) || $11<mn[$1]) mn[$1]=$11
}
END{
    for (k in n)
        printf "%s,%d,%.9f,%.9f,%.9f,%.9f,%.9f\n",
               k,n[k],t[k]/n[k],e[k]/n[k],d[k]/n[k],mx[k],mn[k]
}' | sort -t, -k5,5g >> "$SUM"

echo
echo "=============================================="
echo "FINAL 128-SIM CONFIRMATION"
cat "$SUM"
echo "=============================================="
echo "Winner:"
sed -n '2p' "$SUM"
echo
echo "Saved:"
echo "  $RAW"
echo "  $SUM"
