#!/usr/bin/env bash
set -euo pipefail

# Robust search over actual scheduler behaviors (not just static hardware settings).
# Requires scheduler.cpp = adaptive harness supplied with this script.

if [[ ! -f scheduler.cpp || ! -f libsim.so || ! -f interfaces.h ]]; then
  echo "Run this from src with scheduler.cpp, libsim.so, and interfaces.h present."
  exit 1
fi

WORKERS="${ADAPT_WORKERS:-4}"
TRAIN_SEEDS="${ADAPT_TRAIN_SEEDS:-0 1 2 3 4 5 6 7}"
VALID_SEEDS="${ADAPT_VALID_SEEDS:-8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31}"
ALL_SEEDS="$TRAIN_SEEDS $VALID_SEEDS"
TMP=.adaptive_tmp
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
  END{for(k in n) printf "%s,%d,%.9f,%.9f,%.9f,%.9f,%.9f\n",k,n[k],t[k]/n[k],e[k]/n[k],d[k]/n[k],mx[k],mn[k]}' \
  | sort -t, -k12,12g >> "$sum"
}

run_grid() {
  local params="$1" seeds="$2" raw="$3" sum="$4" label="$5"
  local jobs="$TMP/jobs.txt"; : > "$jobs"
  while read -r mode pn st sd lt sp bp ww; do
    [[ -z "${mode:-}" ]] && continue
    for seed in $seeds; do echo "$mode $pn $st $sd $lt $sp $bp $ww $seed" >> "$jobs"; done
  done < "$params"
  echo "$label ($(wc -l < "$jobs" | tr -d ' ') simulations)"
  echo "$RAW_HEADER" > "$raw"
  xargs -P "$WORKERS" -n 9 bash -c 'run_one "$@"' _ < "$jobs" >> "$raw"
  aggregate "$raw" "$sum"
  echo "  best mean EDP: $(sed -n '2p' "$sum" | awk -F, '{printf "%.6f",$12}')"
}

# Columns in params: mode preemptN sizeT sizeDir loadT fastSmallP fastBigP workWake
P1="$TMP/phase1.params"; : > "$P1"
# Robust baseline: non-preemptive SJF, P4 small / P3 big, wake whenever work waits.
echo "0 1 15000 0 4 3 2 15000" >> "$P1"
# Effective time quantum / SRTF. QUANTUM itself stays 1000; we preempt every N timer ticks.
for n in 1 4 16 64 256; do echo "1 $n 15000 0 4 3 2 15000" >> "$P1"; done
# Size-aware two-class scheduling (logical multi-queue behavior).
for t in 12000 15000 18000; do for dir in 0 1; do echo "2 1 $t $dir 4 3 2 15000" >> "$P1"; done; done
# Adaptive DVFS: low-load baseline is always small=P4, big=P3; these are high-load states.
for lt in 1 2 4 8 16; do
  for pair in "4 2" "3 3" "3 2"; do read -r sp bp <<< "$pair"; echo "3 1 15000 0 $lt $sp $bp 15000" >> "$P1"; done
done
# Wake decisions based on total queued remaining work rather than just process count.
for w in 0 12000 15000 20000 30000 45000 60000; do echo "4 1 15000 0 4 3 2 $w" >> "$P1"; done

run_grid "$P1" "$TRAIN_SEEDS" results_adaptive_primitives_raw.csv results_adaptive_primitives_summary.csv \
  "Phase 1/3: adaptive scheduler primitives"

# Pull the best setting from each primitive family, then test all on/off combinations.
best_for_mode() { awk -F, -v m="$1" 'NR>1 && $1==m {print; exit}' results_adaptive_primitives_summary.csv; }
B1=$(best_for_mode 1); B2=$(best_for_mode 2); B3=$(best_for_mode 3); B4=$(best_for_mode 4)
IFS=, read -r _ PN _ _ _ _ _ _ _rest <<< "$B1"
IFS=, read -r _ _ ST SD _ _ _ _ _rest <<< "$B2"
IFS=, read -r _ _ _ _ LT SP BP _ _rest <<< "$B3"
IFS=, read -r _ _ _ _ _ _ _ WW _rest <<< "$B4"

P2="$TMP/phase2.params"; : > "$P2"
# Mode 5 combines features. Off-values: huge preemption interval, sizeT=0,
# base DVFS (P4/P3), and workWake=0.
for pre_on in 0 1; do
  for size_on in 0 1; do
    for dvfs_on in 0 1; do
      for wake_on in 0 1; do
        [[ $pre_on -eq 1 ]] && pn="$PN" || pn=1000000
        if [[ $size_on -eq 1 ]]; then st="$ST"; sd="$SD"; else st=0; sd=0; fi
        if [[ $dvfs_on -eq 1 ]]; then lt="$LT"; sp="$SP"; bp="$BP"; else lt=999999; sp=4; bp=3; fi
        [[ $wake_on -eq 1 ]] && ww="$WW" || ww=0
        echo "5 $pn $st $sd $lt $sp $bp $ww" >> "$P2"
      done
    done
  done
done
run_grid "$P2" "$TRAIN_SEEDS" results_adaptive_combos_raw.csv results_adaptive_combos_summary.csv \
  "Phase 2/3: combinations of the best adaptive features"

# Merge training summaries, take best 10 unique configurations, validate unseen seeds.
{ tail -n +2 results_adaptive_primitives_summary.csv; tail -n +2 results_adaptive_combos_summary.csv; } \
  | sort -t, -k12,12g \
  | awk -F, '!seen[$1 FS $2 FS $3 FS $4 FS $5 FS $6 FS $7 FS $8]++' \
  | head -10 \
  | awk -F, 'BEGIN{OFS=" "}{print $1,$2,$3,$4,$5,$6,$7,$8}' > "$TMP/top10.params"

run_grid "$TMP/top10.params" "$VALID_SEEDS" results_adaptive_validation_raw.csv results_adaptive_validation_summary.csv \
  "Phase 3/3: unseen-seed validation"

# Final all-seed ranking of validated candidates.
run_grid "$TMP/top10.params" "$ALL_SEEDS" results_adaptive_allseeds_raw.csv results_adaptive_allseeds_summary.csv \
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
echo
echo "Files to send back:"
echo "  results_adaptive_primitives_summary.csv"
echo "  results_adaptive_combos_summary.csv"
echo "  results_adaptive_validation_summary.csv"
echo "  results_adaptive_allseeds_summary.csv"
