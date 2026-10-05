# Scheduler implementation and measured results

October 4, 2026

The implemented scheduler reduced the default workload's final energy counter by **84.8389%**. It completed every process and matched the derived minimum for full-timer-interval execution, including initial idle energy. This is a result for the supplied simulator and its timer-grid workloads, rather than a claim about arbitrary hardware.

## Default workload: seed 0

| Metric | Original starter | Implemented scheduler |
| --- | ---: | ---: |
| Final raw energy | 7487041200 | 1135116600 |
| Existing kWh display conversion | 2.079733666667 | 0.315310166667 |
| Final raw clock | 109121000 | 113310000 |
| Created / completed processes | 6666 / 6666 | 6666 / 6666 |
| Mean turnaround, raw clock units | 22794907.441 | 15866152.865 |
| Maximum turnaround, raw clock units | 41548000 | 29887000 |

Total runtime increased **3.8389%**. Mean turnaround fell **30.3961%**, and maximum turnaround fell **28.0663%**. The objective is total energy, so the default favors the lower-energy configuration despite its slightly longer final drain.

The kWh values above use the starter's division by 3600000000. The raw clock and physical-energy labels remain inconsistent in the supplied simulator, as explained in the [design plan](energy-aware-scheduler-design.md). Percentage comparisons do not depend on resolving a constant unit conversion.

## Implemented policy

The [scheduler](../src/scheduler.cpp) uses a shared FIFO ready queue and explicit ownership/state records for eight cores:

1. Run admitted jobs on cores 4–7, the four small cores. Keep big cores in C6.
2. Use P3 for normal execution, because it minimizes active energy per unit of work in the measured table.
3. At a timer boundary, use P4 when no more than 120 work units remain. Both states finish within the next interval; P4 costs 1800 rather than 2800 raw energy units.
4. Put an emptied core immediately in C6. Request a single wake when the queue needs it, and count pending wakes as future capacity.
5. Load work only after C1 is confirmed. Complete a process using the hardware's C1 reset, without attempting another C0-only save.

There is no future-arrival inspection, seed-specific scheduling rule, artificial workload reduction, or additional timer stream. FIFO jobs run to completion; the only context restart is a justified P-state adjustment for the final interval. Initial C6 waking introduces latency but accrues no per-core wake energy in this library.

Four small cores won the tie among the minimum-energy candidates: one, two, three, and four consumed exactly the same seed-0 energy, but four completed sooner. The model has no positive idle cost for sleeping cores and no explicit cost for a C6 wake, so using additional small cores does not add activation energy.

## Why the final-interval adjustment matters

The timer advances useful work in discrete units. A P3 small core supplies 240 work units in one 1000-unit interval and consumes 2800 energy units. P4 supplies 120 and consumes 1800. P4 is less efficient for sustained work, but cheaper when either state can complete the short remainder in one interval.

On seed 0, the adjustment occurred 2819 times and saved exactly **2819000 raw energy units**, compared with fixed P3, with unchanged completion time. Applying P4 to every interval instead increased energy to 0.404991500 displayed kWh.

The library's running-core `SetPState` routine also accounts for elapsed work. During `TimerInterrupt`, the preceding CPU handler has already credited that interval, while the later handler has not yet reset the execution timestamp. A direct P-state change can credit the work twice. The implementation saves the context, reloads it, and calls `RunCore` before setting the new P-state, resetting that timestamp without consuming simulated time. Independent checks verify that scheduler callbacks do not change remaining work.

## Derived energy minimum at the simulator's timer granularity

For full intervals on a small core:

| State | Work per interval | Energy per interval | Lower-energy replacement |
| --- | ---: | ---: | --- |
| P0 | 600 | 11000 | Two P3 intervals + one P4: 600 work for 7400 energy |
| P1 | 480 | 8000 | Two P3 intervals: 480 work for 5600 energy |
| P2 | 360 | 5400 | One P3 + one P4: 360 work for 4600 energy |
| P3 | 240 | 2800 | Retain |
| P4 | 120 | 1800 | Retain for a short final remainder |

Every big-core interval also has a small-core replacement providing at least as much work at equal or lower energy, when extra runtime is permitted. For example, big P3 delivers 400 work for 5600 energy, while two small P3 intervals deliver 480 for the same energy. Thus big cores and small P0–P2 cannot improve the full-interval energy minimum.

Two P4 intervals are more expensive than one P3 for the same work. Consequently, for a positive-work job of size `W`, an optimal sequence uses P3 and at most one P4 interval:

```text
q = floor(W / 240)
r = W mod 240

E_job_min = 2800*q + {
    0,     if r == 0
    1800,  if 0 < r <= 120
    2800,  if 120 < r < 240
}
```

A zero-work process can finish immediately during its wake-time hardware check; the dedicated regression covers it. For the tested workloads, all arrivals are on the timer grid: `AddProcess` scales both arrival and work inputs by 1000.

Before the first scheduler callback, the eight C1 cores consume a combined rate of 57.6. That energy is unavoidable through the scheduler interface. Therefore the benchmark's reference minimum is:

```text
E_min = sum(E_job_min) + 57.6 * time_of_first_scheduler_callback
```

For seed 0:

```text
Per-job full-interval minimum:  1135059000
Initial idle energy:              57600
Total reference minimum:       1135116600
Measured final counter:        1135116600
```

The result is 0.399043% above the more optimistic continuous-work lower bound of 1130605000. That difference is explained by per-job timer granularity and initial idle residence. Every final-policy validation case matched the full-interval reference, to the test's 0.01-raw-unit tolerance.

This derivation assumes the inspected rates, correct useful-work accounting, timer-grid events, no per-process deadline, and this library's zero-cost C6 residence/wake accounting. Different transition charges, off-grid event sources, or required service deadlines change the optimization problem.

## Generator seeds and held-out results

Only seed 0 was used to choose the default configuration. Seeds 1–5 were then checked without changing the policy. Each run created and completed 6666 processes.

| Seed | Starter displayed kWh | Final displayed kWh | Energy saved | Runtime change |
| --- | ---: | ---: | ---: | ---: |
| 0 | 2.079733667 | 0.315310167 | 84.8389% | +3.8389% |
| 1 | 2.085448778 | 0.315029389 | 84.8939% | +3.9818% |
| 2 | 2.073615111 | 0.313276556 | 84.8923% | +3.9109% |
| 3 | 2.098563222 | 0.315271167 | 84.9768% | +3.4524% |
| 4 | 2.086142556 | 0.313392000 | 84.9774% | +3.8952% |
| 5 | 2.088584889 | 0.314786611 | 84.9282% | +3.6825% |

Across all six seeds, the final policy completed **39996 generated processes**. Each final counter equaled the derived full-interval minimum plus measured initial idle energy.

## Policy sweep

These are measured seed-0 results for all 17 configurations, including the reference. “tail” denotes P3 with the short-final-interval P4 adjustment.

| Policy | Displayed kWh | Final raw clock |
| --- | ---: | ---: |
| starter | 2.079733667 | 109121000 |
| small1-P3-C6 | 0.316093222 | 408388000 |
| small1-tail-C6 | 0.315310167 | 408388000 |
| small2-P3-C6 | 0.316093222 | 205218000 |
| small2-tail-C6 | 0.315310167 | 205218000 |
| small3-P3-C6 | 0.316093222 | 137478000 |
| small3-tail-C6 | 0.315310167 | 137478000 |
| small4-P3-C6 | 0.316093222 | 113310000 |
| small4-tail-C6 | 0.315310167 | 113310000 |
| small4-P0-C6 | 0.500241000 | 88150000 |
| small4-P1-C6 | 0.455791556 | 92833000 |
| small4-P2-C6 | 0.407936500 | 99627000 |
| small4-P4-C6 | 0.404991500 | 204536000 |
| small4-tail-C1 | 0.367116833 | 111311000 |
| small4-tail-C4 | 0.323952611 | 111320000 |
| big1-P3-C6 | 0.379514000 | 245966000 |
| mixed8-tail-C6 | 0.353580833 | 87020000 |

Keeping unused small cores in C1 or C4 improved wake responsiveness but consumed more idle energy. Faster P-states and mixed big/small execution improved runtime at higher energy. Those alternatives remain selectable with compile-time controls when a latency target is introduced.

## Correctness and synthetic cases

The test harness independently wraps context loads/saves to check ownership, checks remaining work before and after callbacks, requires exact completion counts, and verifies the full-interval minimum for the default policy.

| Scenario | Completed processes | Measured raw energy | Reference minimum including startup |
| --- | ---: | ---: | ---: |
| empty | 0 | 57600 | 57600 |
| single | 1 | 13000 | 13000 |
| long | 1 | 116667600 | 116667600 |
| burst | 32 | 43142200 | 43142200 |
| gaps | 12 | 567200 | 567200 |
| mixed | 101 | 121902200 | 121902200 |
| boundary | 80 | 3274200 | 3274200 |
| late | 1 | 70600 | 70600 |
| zero | 2 | 13000 | 13000 |

These cover empty input, one tiny job, a long job, concurrent arrivals, long idle gaps, mixed sizes, arrivals around wake-countdown boundaries, a late first arrival, and zero-work creation. The six generator runs plus these cases complete **40226 final-policy processes** with all checks passing.

Builds used GNU C++17 with `-O2 -Wall -Wextra -Werror`. The scheduler also passed a separate direct build and run using the library's original main function.

## Reproduction and artifacts

On a Linux lab machine:

```sh
cd src
make run
make test
make benchmark
```

The [README](../README.md) also includes the tested read-only Docker recipe for `make test` on Apple Silicon. Benchmark compilation uses an isolated temporary directory and preserves the workload and simulator library.

The [machine-readable measurements](scheduler-benchmark-results.json) contain all sweep and validation records, raw counters, completion counts, minimum calculations, and source/library hashes. The [benchmark driver](../tests/benchmark_driver.cpp), [reference starter](../tests/reference_starter.cpp), and [runner](../tests/run_benchmarks.sh) reproduce the comparisons.

Simulator SHA-256:

```text
920e35872ffeb5ea3ed80fae7db343d86ffbc3f3a1f20e659d939bb9bc731f4b
```

