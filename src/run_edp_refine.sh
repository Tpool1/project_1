#!/usr/bin/env bash
set -eu

# Focused EDP refinement search for the parameterized scheduler.cpp.
# Run this from the project src directory.

if [[ ! -f scheduler.cpp || ! -f libsim.so ]]; then
  echo "Run this script from the project src directory containing scheduler.cpp and libsim.so."
  exit 1
fi

WORKERS="${EDP_WORKERS:-4}"
HEADER="small,big,small_idle,big_idle,small_first,initial_awake,small_p,big_p,policy,wake_threshold,time_h,energy_kwh,edp"
TMPDIR_EDP=".edp_refine_tmp"
rm -rf "$TMPDIR_EDP"
mkdir -p "$TMPDIR_EDP"

cleanup() {
  rm -rf "$TMPDIR_EDP"
}
trap cleanup EXIT

echo "Compiling..."
g++ -std=gnu++17 -O2 -o sim scheduler.cpp -L. -lsim '-Wl,-rpath,$ORIGIN'

run_one() {
  local small="$1" big="$2" small_idle="$3" big_idle="$4" order="$5"
  local awake="$6" small_p="$7" big_p="$8" policy="$9" wake="${10}"
  local out line

  if ! out=$( \
    TEST_NUM_SMALL="$small" \
    TEST_NUM_BIG="$big" \
    TEST_SMALL_IDLE="$small_idle" \
    TEST_BIG_IDLE="$big_idle" \
    TEST_SMALL_FIRST="$order" \
    TEST_INITIAL_AWAKE="$awake" \
    TEST_SMALL_P="$small_p" \
    TEST_BIG_P="$big_p" \
    TEST_POLICY="$policy" \
    TEST_WAKE="$wake" \
    ./sim 2>&1 ); then
      echo "Simulation failed: $small $big $small_idle $big_idle $order $awake $small_p $big_p $policy $wake" >&2
      echo "$out" >&2
      return 1
  fi

  line=$(printf '%s\n' "$out" | awk '/^RESULT,/ {sub(/^RESULT,/, ""); print; exit}')
  if [[ -z "$line" ]]; then
    echo "No RESULT line for: $small $big $small_idle $big_idle $order $awake $small_p $big_p $policy $wake" >&2
    echo "$out" >&2
    return 1
  fi

  printf '%s\n' "$line"
}
export -f run_one

run_params_file() {
  local label="$1" params="$2" outfile="$3"
  local n
  n=$(wc -l < "$params" | tr -d ' ')
  echo "$label ($n runs)"
  echo "$HEADER" > "$outfile"

  # Each parameter row has exactly 10 whitespace-separated integers.
  # xargs runs independent simulator processes in parallel.
  xargs -P "$WORKERS" -n 10 bash -c 'run_one "$@"' _ < "$params" \
    | sort -t, -k13,13g >> "$outfile"

  local best
  best=$(sed -n '2p' "$outfile")
  if [[ -n "$best" ]]; then
    echo "  best EDP so far in this phase: $(printf '%s\n' "$best" | awk -F, '{printf "%.6f", $13}')"
  fi
}

# Combine CSV bodies, sort by EDP, and retain the best row for each exact
# 10-field configuration key. Output has no header.
dedupe_ranked() {
  local f
  for f in "$@"; do
    tail -n +2 "$f"
  done \
    | sort -t, -k13,13g \
    | awk -F, '{key=$1 FS $2 FS $3 FS $4 FS $5 FS $6 FS $7 FS $8 FS $9 FS $10; if (!seen[key]++) print}'
}

# ---------------------------------------------------------------------------
# Phase 1: hardware + P-state refinement around the promising region.
# Mixed layouts use at least 5 total candidate cores. Pure 4-small and
# 4-big controls are also included. P2/P3/P4 are tested independently.
# ---------------------------------------------------------------------------
P1_PARAMS="$TMPDIR_EDP/phase1.params"
: > "$P1_PARAMS"

for s in 1 2 3 4; do
  for b in 1 2 3 4; do
    (( s + b >= 5 )) || continue
    for order in 0 1; do
      for sp in 2 3 4; do
        for bp in 2 3 4; do
          echo "$s $b 6 6 $order 1 $sp $bp 0 1" >> "$P1_PARAMS"
        done
      done
    done
  done
done

# Pure 4-small control.
for sp in 2 3 4; do
  echo "4 0 6 6 1 1 $sp 0 0 1" >> "$P1_PARAMS"
done
# Pure 4-big control.
for bp in 2 3 4; do
  echo "0 4 6 6 1 1 0 $bp 0 1" >> "$P1_PARAMS"
done

sort -u "$P1_PARAMS" -o "$P1_PARAMS"
run_params_file "Phase 1/4: hardware + P-state refinement" "$P1_PARAMS" "results_refine_phase1.csv"

# ---------------------------------------------------------------------------
# Phase 2: independently choose C4 or C6 for small and big cores on the
# top 12 phase-1 configurations.
# ---------------------------------------------------------------------------
P2_PARAMS="$TMPDIR_EDP/phase2.params"
: > "$P2_PARAMS"

sed -n '2,13p' results_refine_phase1.csv | while IFS=, read -r s b _si _bi order _awake sp bp _policy _wake _time _energy _edp; do
  for si in 4 6; do
    for bi in 4 6; do
      echo "$s $b $si $bi $order 1 $sp $bp 0 1"
    done
  done
done > "$P2_PARAMS"
sort -u "$P2_PARAMS" -o "$P2_PARAMS"
run_params_file "Phase 2/4: separate big/small idle states" "$P2_PARAMS" "results_refine_phase2.csv"

# ---------------------------------------------------------------------------
# Phase 3: sweep how many candidate cores start awake for the best 6
# configurations seen in phases 1-2.
# ---------------------------------------------------------------------------
P3_SEEDS="$TMPDIR_EDP/phase3.seeds"
dedupe_ranked results_refine_phase1.csv results_refine_phase2.csv | sed -n '1,6p' > "$P3_SEEDS"

P3_PARAMS="$TMPDIR_EDP/phase3.params"
: > "$P3_PARAMS"
while IFS=, read -r s b si bi order _awake sp bp _policy _wake _time _energy _edp; do
  total=$((s + b))
  for ((awake=1; awake<=total; awake++)); do
    echo "$s $b $si $bi $order $awake $sp $bp 0 1" >> "$P3_PARAMS"
  done
done < "$P3_SEEDS"
sort -u "$P3_PARAMS" -o "$P3_PARAMS"
run_params_file "Phase 3/4: initial-awake sweep" "$P3_PARAMS" "results_refine_phase3.csv"

# ---------------------------------------------------------------------------
# Phase 4: policy x wake-threshold sweep on the top 3 configurations seen
# in phases 1-3.
# policy: 0=FCFS, 1=SJF, 2=LJF, 3=big-long/small-short, 4=control opposite.
# ---------------------------------------------------------------------------
P4_SEEDS="$TMPDIR_EDP/phase4.seeds"
dedupe_ranked results_refine_phase1.csv results_refine_phase2.csv results_refine_phase3.csv | sed -n '1,3p' > "$P4_SEEDS"

P4_PARAMS="$TMPDIR_EDP/phase4.params"
: > "$P4_PARAMS"
while IFS=, read -r s b si bi order awake sp bp _policy _wake _time _energy _edp; do
  for policy in 0 1 2 3 4; do
    for wake in 1 2 3 4 5 6 7 8; do
      echo "$s $b $si $bi $order $awake $sp $bp $policy $wake" >> "$P4_PARAMS"
    done
  done
done < "$P4_SEEDS"
sort -u "$P4_PARAMS" -o "$P4_PARAMS"
run_params_file "Phase 4/4: scheduling policy + wake threshold" "$P4_PARAMS" "results_refine_phase4.csv"

# ---------------------------------------------------------------------------
# Final ranking and deterministic verification.
# ---------------------------------------------------------------------------
RANKED="$TMPDIR_EDP/ranked.csv"
dedupe_ranked results_refine_phase1.csv results_refine_phase2.csv results_refine_phase3.csv results_refine_phase4.csv > "$RANKED"

BEST=$(sed -n '1p' "$RANKED")
IFS=, read -r S B SI BI ORDER AWAKE SP BP POLICY WAKE TIME ENERGY EDP <<< "$BEST"

V1=$(run_one "$S" "$B" "$SI" "$BI" "$ORDER" "$AWAKE" "$SP" "$BP" "$POLICY" "$WAKE")
V2=$(run_one "$S" "$B" "$SI" "$BI" "$ORDER" "$AWAKE" "$SP" "$BP" "$POLICY" "$WAKE")
{
  echo "$HEADER"
  echo "$V1"
  echo "$V2"
} > results_refine_validation.csv

echo
echo "============================================================"
echo "BEST RESULT"
echo "$HEADER"
echo "$BEST"
echo "============================================================"
echo "TOP 15"
echo "$HEADER"
sed -n '1,15p' "$RANKED"
echo
echo "Validation repeats:"
echo "$V1"
echo "$V2"
echo
echo "CSV files saved as results_refine_phase1.csv ... results_refine_phase4.csv"
