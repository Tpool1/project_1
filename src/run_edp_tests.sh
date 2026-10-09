#!/usr/bin/env bash
set -euo pipefail

g++ -std=gnu++17 -O2 -o sim scheduler.cpp -L. -lsim -Wl,-rpath,'$ORIGIN'

run_one() {
    local small="$1" big="$2" idle="$3" order="$4"
    local small_p="$5" big_p="$6" policy="$7" wake="$8"
    TEST_NUM_SMALL="$small" TEST_NUM_BIG="$big" TEST_IDLE="$idle" \
    TEST_SMALL_FIRST="$order" TEST_SMALL_P="$small_p" TEST_BIG_P="$big_p" \
    TEST_POLICY="$policy" TEST_WAKE="$wake" \
    ./sim | awk -F, '/^RESULT,/ {print substr($0,8)}'
}

header="small,big,idle,small_first,small_p,big_p,policy,wake_threshold,time_h,energy_kwh,edp"

# 1) Hardware scan at P3, because P3 is the promising EDP region.
HW=results_hw_p3.csv
echo "$header" > "$HW"
for small in 0 1 2 3 4; do
  for big in 0 1 2 3 4; do
    (( small + big == 0 )) && continue
    for idle in 4 6; do
      if (( small > 0 && big > 0 )); then orders=(1 0); else orders=(1); fi
      for order in "${orders[@]}"; do
        run_one "$small" "$big" "$idle" "$order" 3 3 0 1 >> "$HW"
      done
    done
  done
done

# Keep the 4 best unique hardware layouts.
TOP=top4_hw.csv
awk -F, 'NR>1 {key=$1 FS $2 FS $3 FS $4; if (!(key in best) || $11 < e[key]) {best[key]=$0; e[key]=$11}} END {for (k in best) print best[k]}' "$HW" \
  | sort -t, -k11,11n | head -4 > "$TOP"

# 2) Independent small/big P-state sweep on those top layouts.
DVFS=results_dvfs.csv
echo "$header" > "$DVFS"
while IFS=, read -r small big idle order _ _ _ _ _ _ _; do
  if (( small > 0 && big > 0 )); then
    for sp in 0 1 2 3 4; do
      for bp in 0 1 2 3 4; do
        run_one "$small" "$big" "$idle" "$order" "$sp" "$bp" 0 1 >> "$DVFS"
      done
    done
  elif (( small > 0 )); then
    for sp in 0 1 2 3 4; do run_one "$small" 0 "$idle" 1 "$sp" 0 0 1 >> "$DVFS"; done
  else
    for bp in 0 1 2 3 4; do run_one 0 "$big" "$idle" 1 0 "$bp" 0 1 >> "$DVFS"; done
  fi
done < "$TOP"

BEST=$( { tail -n +2 "$HW"; tail -n +2 "$DVFS"; } | sort -t, -k11,11n | head -1 )
IFS=, read -r S B IDLE ORDER SP BP _ _ _ _ _ <<< "$BEST"

# 3) Scheduling policy sweep.
POL=results_policy.csv
echo "$header" > "$POL"
for policy in 0 1 2 3 4; do
  run_one "$S" "$B" "$IDLE" "$ORDER" "$SP" "$BP" "$policy" 1 >> "$POL"
done
BESTPOL=$(tail -n +2 "$POL" | sort -t, -k11,11n | head -1)
IFS=, read -r S B IDLE ORDER SP BP POLICY _ _ _ _ <<< "$BESTPOL"

# 4) Wake-threshold sweep.
WAKE=results_wake.csv
echo "$header" > "$WAKE"
for w in 1 2 3 4 5 6 7 8; do
  run_one "$S" "$B" "$IDLE" "$ORDER" "$SP" "$BP" "$POLICY" "$w" >> "$WAKE"
done

FINAL=$( { tail -n +2 "$HW"; tail -n +2 "$DVFS"; tail -n +2 "$POL"; tail -n +2 "$WAKE"; } | sort -t, -k11,11n | head -1 )

echo "BEST RESULT"
echo "$header"
echo "$FINAL"
echo
echo "TOP 10"
{ tail -n +2 "$HW"; tail -n +2 "$DVFS"; tail -n +2 "$POL"; tail -n +2 "$WAKE"; } | sort -t, -k11,11n | head -10
