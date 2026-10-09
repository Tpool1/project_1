#!/usr/bin/env bash
set -euo pipefail

# Resume the adaptive search from existing phase 1 and phase 2 summaries.
# Run from src, with scheduler.cpp set to the adaptive harness.

for f in scheduler.cpp libsim.so interfaces.h results_adaptive_primitives_summary.csv results_adaptive_combos_summary.csv; do
  if [[ ! -f "$f" ]]; then
    echo "Missing required file: $f"
    exit 1
  fi
done

WORKERS="${ADAPT_WORKERS:-4}"
VALID_SEEDS="${ADAPT_VALID_SEEDS:-8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31}"
TRAIN_SEEDS="${ADAPT_TRAIN_SEEDS:-0 1 2 3 4 5 6 7}"
ALL_SEEDS="$TRAIN_SEEDS $VALID_SEEDS"

TMP=.adaptive_resume_tmp
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

echo "Compiling adaptive scheduler..."
g++ -std=gnu++17 -O2 -I. -o "$TMP/sim_adapt" scheduler.cpp "$TMP/seed_main.cpp" \
  -L. -lsim '-Wl,-rpath,$ORIGIN/..'

SIM="$TMP/sim_adapt"
export SIM

RAW_HEADER="mode,preempt_n,size_t,size_dir,load_t,fast_small_p,fast_big_p,work_wake,seed,time_h,energy_kwh,edp"
SUM_HEADER="mode,preempt_n,size_t,size_dir,load_t,fast_small_p,fast_big_p,work_wake,nseeds,mean_time_h,mean_energy_kwh,mean_edp,max_edp,min_edp"

run_one() {
  local mode="$1" pn="$2" st="$3" sd="$4" lt="$5" sp="$6" bp="$7" ww="$8" seed="$9"
  local out line
  out=$(ADAPT_MODE="$mode" ADAPT_PREEMPT_N="$pn" ADAPT_SIZE_T="$st" ADAPT_SIZE_DIR="$sd" \
        ADAPT_LOAD_T="$lt" ADAPT_FAST_SMALL_P="$sp" ADAPT_FAST_BIG_P="$bp" \
        ADAPT_WORK_WAKE="$ww" "$SIM" "$seed")
  line=$(printf '%s\n' "$out" | awk -F, '/^ADAPT_RESULT,/ {print; exit}')
  [[ -n "$line" ]] || { echo "No result for seed=$seed config=$*" >&2; return 1; }
  printf '%s\n' "$line" | awk -F, -v seed="$seed" 'BEGIN{OFS=","}{print $2,$3,$4,$5,$6,$7,$8,$9,seed,$10,$11,$12}'
}
export -f run_one

aggregate() {
  local raw="$1" sum="$2"
  echo "$SUM_HEADER" > "$sum"
  tail -n +2 "$raw" | awk -F, 'BEGIN{OFS=","}
  {
    key=$1 FS $2 FS $3 FS $4 FS $5 FS $6 FS $7 FS $8;
    n[key]++; t[key]+=$10; e[key]+=$11; d[key]+=$12;
    if (!(key in mx) || $12>mx[key]) mx[key]=$12;
    if (!(key in mn) || $12<mn[key]) mn[key]=$12;
  }
  END{
    for(k in n)
      printf "%s,%d,%.9f,%.9f,%.9f,%.9f,%.9f\n",
             k,n[k],t[k]/n[k],e[k]/n[k],d[k]/n[k],mx[k],mn[k]
  }' | sort -t, -k12,12g >> "$sum"
}

run_grid() {
  local params="$1" seeds="$2" raw="$3" sum="$4" label="$5"
  local jobs="$TMP/jobs.txt"
  : > "$jobs"

  while read -r mode pn st sd lt sp bp ww; do
    [[ -z "${mode:-}" ]] && continue
    for seed in $seeds; do
      echo "$mode $pn $st $sd $lt $sp $bp $ww $seed" >> "$jobs"
    done
  done < "$params"

  echo "$label ($(wc -l < "$jobs" | tr -d ' ') simulations)"
  echo "$RAW_HEADER" > "$raw"
  xargs -P "$WORKERS" -n 9 bash -c 'run_one "$@"' _ < "$jobs" >> "$raw"
  aggregate "$raw" "$sum"
  echo "  best mean EDP: $(sed -n '2p' "$sum" | awk -F, '{printf "%.6f",$12}')"
}

# Merge the already-completed phase 1 and phase 2 summaries.
# sed -n '1,10p' deliberately reads the whole stream, avoiding the pipefail/head SIGPIPE issue.
{
  tail -n +2 results_adaptive_primitives_summary.csv
  tail -n +2 results_adaptive_combos_summary.csv
} \
  | sort -t, -k12,12g \
  | awk -F, '!seen[$1 FS $2 FS $3 FS $4 FS $5 FS $6 FS $7 FS $8]++' \
  | sed -n '1,10p' \
  | awk -F, 'BEGIN{OFS=" "}{print $1,$2,$3,$4,$5,$6,$7,$8}' \
  > "$TMP/top10.params"

echo "Resuming after completed phases 1 and 2..."

run_grid "$TMP/top10.params" "$VALID_SEEDS" \
  results_adaptive_validation_raw.csv results_adaptive_validation_summary.csv \
  "Phase 3/3: unseen-seed validation"

run_grid "$TMP/top10.params" "$ALL_SEEDS" \
  results_adaptive_allseeds_raw.csv results_adaptive_allseeds_summary.csv \
  "Final: all-seed ranking"

echo
echo "============================================================"
echo "BEST ADAPTIVE RESULT (seeds 0-31)"
echo "$SUM_HEADER"
sed -n '2p' results_adaptive_allseeds_summary.csv
echo "============================================================"
echo "TOP 10"
echo "$SUM_HEADER"
sed -n '2,11p' results_adaptive_allseeds_summary.csv
