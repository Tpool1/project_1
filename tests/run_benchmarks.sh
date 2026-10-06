#!/bin/sh
# Run on Linux, as required by the supplied ELF simulator library.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
benchmark_dir=$(mktemp -d "${TMPDIR:-/tmp}/eec-scheduler-benchmark.XXXXXX")
trap 'rm -f "$benchmark_dir/driver.o" "$benchmark_dir/policy.o" "$benchmark_dir/ready_queue.o" "$benchmark_dir/benchmark" "$benchmark_dir/placement-test"; rmdir "$benchmark_dir"' EXIT HUP INT TERM
compiler=${CXX:-g++}
renames='-DCreateProcess=PolicyCreateProcess -DExitProcess=PolicyExitProcess -DTimerInterrupt=PolicyTimerInterrupt -DCStateTransitionComplete=PolicyCStateTransitionComplete -DSimulationComplete=PolicySimulationComplete'
mode=${1:---check}

case "$mode" in
    --check) audit=-DEEC_CHECK_CALLBACK_WORK ;;
    --sweep|--placement-sweep) audit= ;;
    *) printf 'Usage: sh tests/run_benchmarks.sh [--check|--sweep|--placement-sweep]\n' >&2; exit 2 ;;
esac

"$compiler" -std=gnu++17 -O2 -Wall -Wextra -Werror $audit -I"$project_dir/src" \
    -c "$project_dir/tests/benchmark_driver.cpp" -o "$benchmark_dir/driver.o"

build_policy() {
    policy_name=$1
    policy_source=$2
    shift 2
    "$compiler" -std=gnu++17 -O2 -Wall -Wextra -Werror -I"$project_dir/src" $renames "$@" \
        -c "$policy_source" -o "$benchmark_dir/policy.o"
    "$compiler" -std=gnu++17 -O2 -Wall -Wextra -Werror -I"$project_dir/src" \
        -c "$project_dir/src/ready_queue.cpp" -o "$benchmark_dir/ready_queue.o"
    "$compiler" "$benchmark_dir/driver.o" "$benchmark_dir/policy.o" "$benchmark_dir/ready_queue.o" \
        -L"$project_dir/src" -lsim -Wl,-rpath,"$project_dir/src" \
        -Wl,--wrap=_Z11LoadContextjj -Wl,--wrap=_Z11SaveContextjj -o "$benchmark_dir/benchmark"
    printf 'POLICY %s\n' "$policy_name"
}

if [ "$mode" = --check ]; then
    "$compiler" -std=gnu++17 -O2 -Wall -Wextra -Werror -I"$project_dir/src" \
        "$project_dir/tests/job_placement_test.cpp" -o "$benchmark_dir/placement-test"
    "$benchmark_dir/placement-test"
    unset EEC_HIGH_PROBABILITIES
    EEC_PLACEMENT_SEED=0
    export EEC_PLACEMENT_SEED
    build_policy final "$project_dir/src/scheduler.cpp"
    for scenario in empty single long burst gaps mixed boundary late zero equal extrema seed:0 seed:1 seed:2 seed:3 seed:4 seed:5; do
        "$benchmark_dir/benchmark" "$scenario"
    done
    # Endpoint curves exercise strict routing, including zero-work big jobs.
    for curve in 0,0,0,0,0 1,1,1,1,1; do
        for scenario in zero burst gaps equal extrema; do
            EEC_HIGH_PROBABILITIES=$curve "$benchmark_dir/benchmark" "$scenario"
        done
    done
    build_policy small-only "$project_dir/src/scheduler.cpp" -DEEC_BIG_CORES=0 -DEEC_IDLE_STATE=C6
    for scenario in empty single long burst gaps mixed boundary late equal extrema seed:0 seed:1 seed:2 seed:3 seed:4 seed:5; do
        "$benchmark_dir/benchmark" "$scenario" --expect-minimum
    done
    # A zero-work job on an initially ready core consumes one interval before
    # the hardware exits it; the zero-cost per-job floor is not attained here.
    "$benchmark_dir/benchmark" zero
    build_policy big-only "$project_dir/src/scheduler.cpp" -DEEC_SMALL_CORES=0
    for scenario in zero burst gaps equal extrema seed:0; do
        "$benchmark_dir/benchmark" "$scenario"
    done
    build_policy starter "$project_dir/tests/reference_starter.cpp"
    for scenario in seed:0 seed:1 seed:2 seed:3 seed:4 seed:5; do
        "$benchmark_dir/benchmark" "$scenario"
    done
elif [ "$mode" = --placement-sweep ]; then
    # All curves share core counts, P/C-states, arrivals, and random draws.
    build_policy placement "$project_dir/src/scheduler.cpp"
    for curve_name in all-small conservative example linear aggressive all-big; do
        case "$curve_name" in
            all-small) curve=0,0,0,0,0 ;;
            conservative) curve=0,0.05,0.15,0.35,0.60 ;;
            example) curve=0.05,0.15,0.35,0.65,0.90 ;;
            linear) curve=0,0.25,0.50,0.75,1 ;;
            aggressive) curve=0.20,0.40,0.65,0.85,1 ;;
            all-big) curve=1,1,1,1,1 ;;
        esac
        for workload_seed in ${EEC_WORKLOAD_SEEDS:-0 1 2 3 4 5}; do
            for placement_seed in ${EEC_PLACEMENT_SEEDS:-0 1 2}; do
                printf 'POLICY placement-%s\n' "$curve_name"
                EEC_HIGH_PROBABILITIES=$curve EEC_PLACEMENT_SEED=$placement_seed \
                    "$benchmark_dir/benchmark" "seed:$workload_seed"
            done
        done
    done
else
    build_policy starter "$project_dir/tests/reference_starter.cpp"
    "$benchmark_dir/benchmark" seed:0
    for count in 1 2 3 4; do
        build_policy "small${count}-P3-C6" "$project_dir/src/scheduler.cpp" -DEEC_BIG_CORES=0 -DEEC_SMALL_CORES=$count -DEEC_TAIL_DVFS=0 -DEEC_IDLE_STATE=C6
        "$benchmark_dir/benchmark" seed:0
        build_policy "small${count}-tail-C6" "$project_dir/src/scheduler.cpp" -DEEC_BIG_CORES=0 -DEEC_SMALL_CORES=$count -DEEC_IDLE_STATE=C6
        "$benchmark_dir/benchmark" seed:0 --expect-minimum
    done
    for state in P0 P1 P2 P4; do
        build_policy "small4-${state}-C6" "$project_dir/src/scheduler.cpp" -DEEC_BIG_CORES=0 -DEEC_PSTATE=$state -DEEC_TAIL_DVFS=0 -DEEC_IDLE_STATE=C6
        "$benchmark_dir/benchmark" seed:0
    done
    for idle in C1 C4; do
        build_policy "small4-tail-${idle}" "$project_dir/src/scheduler.cpp" -DEEC_BIG_CORES=0 -DEEC_IDLE_STATE=$idle
        "$benchmark_dir/benchmark" seed:0
    done
    build_policy big1-P3-C6 "$project_dir/src/scheduler.cpp" -DEEC_SMALL_CORES=0 -DEEC_BIG_CORES=1 -DEEC_TAIL_DVFS=0 -DEEC_IDLE_STATE=C6
    "$benchmark_dir/benchmark" seed:0
    build_policy mixed8-tail-C6 "$project_dir/src/scheduler.cpp" -DEEC_BIG_CORES=4 -DEEC_IDLE_STATE=C6
    "$benchmark_dir/benchmark" seed:0
fi
