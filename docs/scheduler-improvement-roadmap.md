# Scheduler latency and throughput improvement roadmap

Recorded October 5, 2026. Status: proposed follow-up work, not implemented.

Keep the current four-small-core P3 policy, P4 final-interval adjustment, and C6 sleeping as the energy-first reference. Improve scheduling order before buying additional performance with higher energy consumption. Implement and benchmark each change independently; none of the projections below should be reported as a measured improvement.

This roadmap supplements the [original design plan](energy-aware-scheduler-design.md) and [implementation results](scheduler-results.md). The [policy-sweep records](scheduler-benchmark-results.json) and [preserved latency diagnostics](scheduler-latency-diagnostics.json) contain the supporting measurements. The diagnostic data were copied from the October 5 temporary harness; they are historical observations, not a new benchmark run performed while writing this document.

## Baseline and constraints

On generator seed 0, the current scheduler completes all 6666 jobs with:

| Metric | Current value |
| --- | ---: |
| Final raw energy | 1135116600 |
| Final raw clock | 113310000 |
| Mean turnaround | 15866152.865 |
| Mean first-dispatch wait | 15805189.019 |
| Maximum first-dispatch wait | 29824000 |

All times and energies here are raw simulator units. The assignment, headers, accounting code, and display conversion have inconsistent physical-unit labels; do not interpret these numbers as independently calibrated seconds or joules.

The four small cores at P3 collectively supply 960 work units per 1000-unit timer interval. Changing job order can improve responsiveness and early completions, but cannot increase this raw processing capacity. The tested policy already matches the derived full-interval energy minimum; genuinely increasing sustained capacity generally requires an explicit energy tradeoff.

The proposed order of work is:

1. Dispatch known work before sleeping initially ready cores.
2. Add fair, timer-boundary shortest-remaining-work preemption.
3. Add bounded, adaptive idle-state control.
4. Activate big cores selectively under a declared energy/service budget.

## 1. Dispatch before sleep during initialization

### Reason and evidence

The current [algorithm initialization routine](../src/energy_aware_algorithm.cpp) sleeps every core before the process-creation event admits its job. If the first callback already contains runnable work, this turns a usable C1 core into a sleeping core and immediately requests its wake.

The isolated 1000-work-unit diagnostic measured arrival at 0, first dispatch at 2000000, completion at 2005000, and raw energy 13000. Only 5000 clock units were useful execution; the initial wake caused the rest of the delay.

### Proposed change

Separate initial bookkeeping from hardware sleeping:

```text
initialize ownership/state records
admit the process, when this is a creation callback
dispatch known ready work onto available C1 cores
sleep only unused cores
```

Do not keep unused cores warm while waiting to discover future arrivals. If the first callback is a timer with no ready work, sleeping the cores remains appropriate. This change addresses an avoidable initial sleep/wake cycle, not later unexpected arrivals after real idle gaps.

### Expected benefit and acceptance test

For the first-arrival-at-zero fixture, completion is projected to fall from 2005000 to approximately 5000, with the same 13000 active energy and no initial C6 wake. This has not yet been benchmarked.

Verify that fixture, simultaneous arrivals, empty input, a late first arrival, and all six generator seeds. Require unchanged useful-work accounting and energy-minimum checks; measure any changed runtime rather than assuming every workload benefits equally.

## 2. Fair shortest-remaining-work preemption

### Reason and evidence

The current ready queue is FIFO and running jobs continue until completion. In the targeted blocking test, four jobs of 10000000 work units arrived at 0 and began at 2000000. A 1000-work-unit job arrived at 3000000 but could not start until 43667000:

```text
tiny-job wait:       40667000
tiny-job execution:      5000
tiny-job completion: 43672000
```

All five jobs completed and energy matched the full-interval minimum of 466683400. The problem is responsiveness, not lost work or excessive measured energy.

### Proposed change

At `TimerInterrupt`, rank ready and running jobs by `GetRemaining` and favor the smallest remaining jobs on the four small cores. Preempt a longer running job when doing so serves a materially shorter queued job. Preserve assignments on ties and avoid unnecessary migrations.

Add protected service opportunities for overdue jobs, using arrival time and time since last service. A simple aging/round-robin fallback is preferable to unbounded short-job preference. Declare the service-gap target and report violations; an overloaded system cannot promise arbitrary waiting bounds for every job.

Preserve P3 for normal work and P4 only when the next interval can finish the remainder at lower energy. Perform preemption at timer boundaries after the CPU has advanced running work, not from a creation or wake callback. Use the legal context-save/load/run sequence, including the existing timestamp-reset protection around P-state changes.

### Expected benefit and acceptance test

With the four cores already running, the tiny job should be eligible at the next timer boundary rather than after all long jobs finish. Energy is expected to remain close to the reference in this simulator because switching does not have an explicit charged overhead and work is still executed in full intervals. Both expectations require measurement.

This is not a claim that shortest-remaining-work plus aging is universally optimal on multiple cores. It targets response time, mean turnaround, and early completions; it does not add raw capacity or guarantee a shorter final drain.

Test the blocking fixture, a sustained stream of tiny jobs alongside long jobs, equal-size jobs, migration, and six paired generator seeds. Check long-job progress, context ownership, remaining-work conservation, preemption count, final energy, and runtime.

## 3. Adaptive idle control and bounded predictive waking

### Reason and evidence

Immediate C6 sleeping is efficient but makes isolated arrivals expensive in latency. The same 1000-work-unit job produced:

| Idle policy for the four enabled small cores | Dispatch wait | Turnaround | Raw energy |
| --- | ---: | ---: | ---: |
| C6 | 2000000 | 2005000 | 13000 |
| C4 | 10000 | 15000 | 57000 |
| C1 | 0 | 5000 | 85000 |

The gap fixture required 12 wakes for 12 jobs and averaged 1999083.333 waiting units. In the full seed-0 sweep, using C4 for all enabled small cores reduced mean turnaround by 12.54% for 2.74% more energy. These measurements do not establish the cost or benefit of keeping only one reserve in C4.

### Proposed change

Keep C6 as the long-gap default. During observed recurring bursts, consider one small-core reserve in C4 for a bounded period, then return it to C6. Avoid permanently keeping all cores shallow-idle.

For sufficiently predictable arrival patterns, estimate the next arrival from past observations and request a C6 wake roughly its measured countdown in advance. Use existing timer callbacks, respect actual transition progress, and never inspect future generator jobs or hard-code a seed or fixture period. Early wakes can incur C1 idle energy after becoming ready; late or missed predictions still incur latency. Gate prediction by confidence, bound speculative wakes, and record their energy cost.

This follows the general principle used by [Linux CPUIdle governors](https://docs.kernel.org/admin-guide/pm/cpuidle.html): idle-state selection considers predicted idle duration and exit-latency constraints. Linux's hardware assumptions and governor implementation are not interchangeable with this simulator.

### Expected benefit and acceptance test

The goal is to reduce repeated cold-wake delays while spending less idle energy than an always-C4 policy. The proposed controller has not been benchmarked, so no saving or latency percentage is promised.

Test regular gaps, jittered gaps, isolated surprise arrivals, changing arrival rates, and long silence after a burst. Compare C6, fixed C4, and the controller. Record wake requests/completions, prediction misses, time spent in C1/C4, final energy, and latency percentiles. Bound reserve duration and ensure outstanding wake requests are never repeatedly reset.

## 4. Budgeted big-core activation for sustained backlog

### Reason and evidence

Additional capacity is necessary when four small cores cannot meet the desired service target. The measured seed-0 alternatives, relative to the current policy, are:

| Fixed measured policy | Energy increase | Mean turnaround reduction | Total runtime reduction |
| --- | ---: | ---: | ---: |
| Four small cores, C4 idle | 2.74% | 12.54% | 1.76% |
| Eight cores, P3 with final-interval adjustment | 12.14% | 85.07% | 23.20% |
| Four small cores, P2 | 29.38% | 45.24% | 12.08% |

The eight-core configuration beats the small-core P2 configuration on both final energy and runtime. This supports testing selective big-core P3 activation before broadly raising small-core frequencies. It does not establish that big-core activation is best for every individual job or burst.

### Proposed change

Measure queued work, oldest waiting time, current capacity, and pending wakes. Estimate whether the existing small cores can clear the backlog within a chosen service target. If not, consider waking one big core at P3, then reassess before requesting another.

Include wake delay when evaluating whether additional capacity will arrive soon enough to help. Count pending wakes as future capacity. Use different activation/deactivation thresholds to avoid oscillation, and return an unused big core to C6 when demand falls. Compare any frequency boost using the full energy table rather than assuming it is cheaper because it uses a small core.

Use a declared energy budget rather than silently changing the energy-only objective. Benchmark budgets of +0%, +3%, +5%, and +10% relative to the current policy on the same trace; these are suggested evaluation points, not assignment requirements. Select the fastest acceptable result within the chosen budget. An online final-energy guarantee would need conservative accounting for committed work and reserve costs; a heuristic threshold alone does not provide that guarantee.

### Expected benefit and acceptance test

Selective activation aims to retain much of the fixed eight-core policy's responsiveness while paying for big cores only during sustained demand. That controller is unimplemented and unmeasured; do not extrapolate the fixed-policy percentages as its expected result.

Test sustained overload, short bursts that end before a wake completes, isolated long jobs, mixed sizes, and held-out seeds. Record big-core work/residence, wake delays, actual energy-budget overshoot, mean and tail latency, and total runtime.

## Common validation and decision gates

Measure each change separately before combining them. Preserve the current policy as a reproducible comparison and use identical arrivals/work for paired runs.

- Require every process to complete exactly once, valid context ownership, and no scheduler callback that artificially changes remaining work.
- Report raw final energy and clock, response time, turnaround, p50/p95/p99 latency, and completed jobs over time. Faster final drain and more early completions are different outcomes.
- For preemption, first-dispatch wait no longer captures all non-executing time. Add per-job service-gap measurements and long-job progress checks rather than reusing that metric as total waiting time.
- Preserve full-interval energy-minimum checks for candidate energy-neutral changes where the derivation still applies. For policies deliberately using idle reserves or big cores, check their declared energy budget instead of expecting equality with the minimum.
- Extend the benchmark harness with the blocking and starvation-pressure fixtures. Its blocking input is four `AddProcess(0, 10000)` calls followed by `AddProcess(3000, 1)`; `AddProcess` scales both arguments by 1000.
- Run `make test` and `make benchmark` from `src`, then add paired candidate runs over seeds 0–5 and non-generator traces. Choose thresholds on development traces and evaluate unchanged parameters on held-out traces.

If final energy must remain at today's minimum, prioritize changes 1 and 2. If a modest explicit energy allowance is acceptable, proceed to 3 and 4 and choose a measured tradeoff rather than a universal preset.

## Reference implementation identity

These observations describe the scheduler at repository commit `447ddbb` (`first pass`), before the roadmap changes are implemented.

```text
scheduler SHA-256: c50031cd8b54a9375ddbd42928aec5133a5cfabdf2c94b99e4293d2600433d35
simulator SHA-256: 920e35872ffeb5ea3ed80fae7db343d86ffbc3f3a1f20e659d939bb9bc731f4b
```
