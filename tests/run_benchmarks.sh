#!/bin/sh
# Run on Linux, as required by the supplied ELF simulator library.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
benchmark_dir=$(mktemp -d "${TMPDIR:-/tmp}/eec-scheduler-benchmark.XXXXXX")
trap 'rm -f "$benchmark_dir/driver.o" "$benchmark_dir/policy.o" "$benchmark_dir/ready_queue.o" "$benchmark_dir/benchmark" "$benchmark_dir/idle_timeout_test"; rmdir "$benchmark_dir"' EXIT HUP INT TERM
compiler=${CXX:-g++}
renames='-DCreateProcess=PolicyCreateProcess -DExitProcess=PolicyExitProcess -DTimerInterrupt=PolicyTimerInterrupt -DCStateTransitionComplete=PolicyCStateTransitionComplete -DSimulationComplete=PolicySimulationComplete'
mode=${1:---check}

case "$mode" in
    --check) audit=-DEEC_CHECK_CALLBACK_WORK ;;
    --sweep) audit= ;;
    *) printf 'Usage: sh tests/run_benchmarks.sh [--check|--sweep]\n' >&2; exit 2 ;;
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
    "$compiler" -std=gnu++17 -O2 -Wall -Wextra -Werror -DEEC_BIG_CORES=0 -DEEC_SMALL_CORES=1 \
        -I"$project_dir/src" "$project_dir/src/scheduler.cpp" \
        "$project_dir/src/ready_queue.cpp" "$project_dir/tests/idle_timeout_test.cpp" \
        -o "$benchmark_dir/idle_timeout_test"
    "$benchmark_dir/idle_timeout_test"
    build_policy final "$project_dir/src/scheduler.cpp"
    for scenario in empty single long burst gaps mixed boundary late zero seed:0 seed:1 seed:2 seed:3 seed:4 seed:5; do
        "$benchmark_dir/benchmark" "$scenario"
    done
    build_policy starter "$project_dir/tests/reference_starter.cpp"
    for scenario in seed:0 seed:1 seed:2 seed:3 seed:4 seed:5; do
        "$benchmark_dir/benchmark" "$scenario"
    done
else
    build_policy starter "$project_dir/tests/reference_starter.cpp"
    "$benchmark_dir/benchmark" seed:0
    for count in 1 2 3 4; do
        build_policy "small${count}-P3-C6" "$project_dir/src/scheduler.cpp" -DEEC_SMALL_CORES=$count -DEEC_TAIL_DVFS=0
        "$benchmark_dir/benchmark" seed:0
        build_policy "small${count}-tail-C6" "$project_dir/src/scheduler.cpp" -DEEC_SMALL_CORES=$count
        "$benchmark_dir/benchmark" seed:0 --expect-minimum
    done
    for state in P0 P1 P2 P4; do
        build_policy "small4-${state}-C6" "$project_dir/src/scheduler.cpp" -DEEC_PSTATE=$state -DEEC_TAIL_DVFS=0
        "$benchmark_dir/benchmark" seed:0
    done
    for idle in C1 C4; do
        build_policy "small4-tail-${idle}" "$project_dir/src/scheduler.cpp" -DEEC_MAX_IDLE_C_STATE=$idle
        "$benchmark_dir/benchmark" seed:0
    done
    build_policy big1-P3-C6 "$project_dir/src/scheduler.cpp" -DEEC_SMALL_CORES=0 -DEEC_BIG_CORES=1 -DEEC_TAIL_DVFS=0
    "$benchmark_dir/benchmark" seed:0
    build_policy mixed8-tail-C6 "$project_dir/src/scheduler.cpp" -DEEC_BIG_CORES=4
    "$benchmark_dir/benchmark" seed:0
fi
