# Predicted capacity and bounded warm reserve

This is the earlier waiting-oriented experiment. The current objective and
implementation are documented in [EDP-only scheduling](pure-edp.md); its mixed
workload tradeoff below has been removed.

Implemented and measured October 8, 2026. Compared with the user's existing
eight-core scheduler, mean dispatch waiting falls **5.10%**, mean turnaround
falls **4.95%**, total energy falls **0.28%**, and batch EDP falls **0.52%**,
averaged over six paired generator seeds. Core counts, SJF, and P3/tail P4 are
identical in each pair. Production defaults remain four small and four big cores.

The scheduler now estimates when running cores will finish, when pending wakes
will complete, and how previously queued jobs occupy that future capacity.
Execution estimates use the simulator's big/small work rates and round up to
full timer intervals. A new C4 wake is requested when its estimated dispatch
time is earlier than the running or waking capacity that would otherwise serve
the queued job. Equal dispatch times favor existing capacity.

This fixes a specific waiting bug: a slow C6 wake counted as sufficient capacity
could prevent an available C4 sleeper from waking for 2,000,000 raw time units,
even though C4 takes only 10,000. The new comparison allows the C4 wake to
overtake that pending C6 wake. Forecasts do not reserve process ownership or
dispatch before the actual transition callback. Pending transitions are never
restarted, and cores already deepening still wait for their real callback.

Early C6 admission is retained when queue length exceeds pending wake count.
The supplied library charges no C6 residence or wake energy; postponing these
requests until existing cores could no longer drain all admitted work delayed
capacity needed by continuing arrivals. Running-work comparisons therefore
guide the additional C4 wakes rather than suppressing cold capacity wholesale.

The arrival stream also determines whether to extend one available core's
warm window. After at least four observations, the scheduler takes the mean
gap between its last eight arrival timestamps. When that gap exceeds
`EEC_IDLE_TIMEOUT` but is at most 100,000 raw units, it keeps one available
core in C4 for up to 100,000 units per idle episode, preferring small cores.
The extension stops after 100,000 units without another arrival. Other cores
retain the normal timeout. No C6 core is woken just to create this reserve.
Cold history, dense bursts already covered by the normal timeout, and long
gaps retain the original budget. As before, asynchronous deepening adds its
countdown to physical C4 residence.

The implementation uses only observed arrival timestamps, admitted work, and
known simulator rates. The 100,000-unit reserve bound is a measured heuristic,
not an optimal forecast or a latency guarantee.

**Measured results.** Percentages below compare the final policy with the
previous eight-core policy on the same workload. Seed summaries are arithmetic
means of the six paired percentage changes, not ratios of pooled averages.
All quantities use raw simulator units.

| Workload | Mean dispatch wait | Mean turnaround | Energy | Batch EDP |
| --- | ---: | ---: | ---: | ---: |
| Generator seeds 0–5, paired mean | −5.10% | −4.95% | −0.28% | −0.52% |
| Seed 0 | −0.0094% | +0.0118% | −0.2464% | −0.2510% |
| Mixed long job and staggered short jobs | −44.68% | −32.21% | +0.36% | +0.36% |
| Pending cold wake followed by reusable C4 capacity | −96.33% | −91.98% | −1.28% | −94.68% |
| Periodic burst boundary case | unchanged | unchanged | unchanged | unchanged |
| Long running job requiring another core | unchanged | unchanged | unchanged | unchanged |

Mean dispatch waiting improves on every tested generator seed, by
0.0094–11.93%. Batch EDP improves on every seed, by 0.165–0.980%. The default
seed's waiting is essentially unchanged; most average responsiveness gains
come from the other seeds. Seed 0's final energy is 1,269,057,600 and final
clock is 87,097,000, with all 6,666 jobs completed.

In the blocked-wake regression, mean dispatch wait falls from 663,000 to
24,333 and final clock falls from 2,023,000 to 109,000. The test checks that
maximum turnaround stays below 100,000. A second regression ensures that a
long running job still triggers useful parallel capacity within the C6 wake
interval. Empty, zero-work, single, long, burst, sparse, short-gap, long-gap,
changing-gap, boundary, and demand-during-deepening cases also pass.

These results do not establish a universal EDP improvement. The mixed case
pays 0.36% extra energy/EDP for lower waiting, and more C4 wakes can trade
energy for responsiveness. No improvement in physical wall-clock execution
time is claimed.

**Verification and reproduction.** All 40 paired performance runs and 27 final
correctness runs passed. The final-source suite independently checks completion,
unique context ownership, unchanged remaining work across callbacks, and the
active-energy lower bound. Its overlapping energy, clock, wait, service, and
turnaround measurements match the paired benchmark results. The simulator's
original entry point also builds cleanly with `-Wall -Wextra -Werror` and
reproduces seed-0 energy and clock.

Run `make -C src test` on Linux, or use the README's isolated Docker recipe.
[Machine-readable results](predicted-capacity-results.json) include all paired
runs, correctness runs, per-seed changes, source hashes, the exact previous
scheduler, and reproduction driver/scripts. The previous source snapshot is
necessary because it includes the user's four-big-core default, which differed
from the earlier seven-core analysis baseline.
