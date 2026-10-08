# EDP-only scheduling

October 8, 2026. The sole optimization metric is:

```text
EDP = raw total energy × final simulation clock
```

Mean dispatch waiting, mean turnaround, and tail latency are diagnostics.
They are not optimization terms or service constraints. A change that serves
a short job sooner is useful only if it improves batch EDP.

The scheduler retains the measured short-stream policy: four small and four
big cores, SJF, P3 with final-interval P4, early C6 admission, C4 wake comparison,
and one bounded reserve based on recent arrivals. This policy was selected by
measured EDP. It preserves the previous six-seed EDP improvement exactly.

When current running work outlasts the 2,000,000-unit C6 wake interval, the
scheduler evaluates the admitted batch's projected energy and absolute finish
clock. The forecast includes accumulated energy, remaining running work, queued
jobs, big/small execution rates, P3/P4 interval costs, idle residence, and pending
transition completion. Wake candidates can include a core still deepening;
that choice waits for its real callback before requesting a wake. Contexts and
remaining work are unchanged by forecasts.

The lower projected product selects the core. This can favor a small core over
an immediately available big core: a long-running job can already determine
batch completion, allowing the cheaper job to run more slowly without extending
the batch. Additional wakes stop when none improves the projected product.

In the same long-running mode, a warm reserve compares its projected batch EDP
with normal sleeping. The optional future job uses mean work and arrival gap
from observed arrivals only. A fixed batch horizon rejects warmth when its
additional energy merely reduces that future job's waiting. The reserve remains
bounded at 100,000 raw units for one available core.

The short-stream recipe is kept because the unrestricted batch forecast worsened
measured generator EDP. Arrival-limited streams have a future horizon that the
admitted batch alone cannot establish. Restricting the forecast to running work
that can hide a cold wake preserves the empirically better policy for those
streams. The forecast is an estimate, not a proof of global optimality.

**Measured EDP.** Three configurations ran the same 20 workloads: generator
seeds 0–5 and fourteen synthetic cases. The controls are the original eight-core
policy before predicted capacity, and the previous waiting-oriented policy.

| Workload | Final EDP versus previous waiting-oriented policy | Final EDP versus original eight-core policy |
| --- | ---: | ---: |
| Seeds 0–5, mean paired percentage | unchanged | −0.5221% |
| Mixed long job and staggered short jobs | **−0.9354%** | **−0.5810%** |
| Long-running job plus parallel job | **−4.4160%** | **−4.4160%** |
| Blocked warm wake | unchanged | −94.6808% |

Final EDP does not increase against either control on any of the twenty tested
workloads. These comparisons cover the tested inputs, not arbitrary arrivals.

The mixed workload's energy falls from 123,192,400 under the previous policy
to 122,040,000. Final clock remains 41,667,000. Its previous 0.36% EDP penalty
versus the original policy is replaced by a 0.581% improvement. Mean waiting
increases 49.29% relative to the previous policy; the lower EDP is the reason
to accept the change.

In the parallel long-job case, energy falls from 158,875,600 to 151,859,600 at
the same final clock, 41,667,000. Choosing a small core saves active energy even
though that parallel job takes longer to execute. Mean turnaround increases
9.77%, while EDP decreases 4.416%.

Generator results exactly match the preceding policy, including seed-0 energy
1,269,057,600, final clock 87,097,000, and all 6,666 jobs completed. The six-seed
0.5221% EDP gain versus the original eight-core policy is retained.

**Verification.** All 60 comparison runs and 27 final correctness runs passed.
Independent checks validate completion, unique context ownership, unchanged
remaining work across callbacks, and the active-energy lower bound. Tests now
enforce EDP ceilings for the blocked-wake, mixed, and parallel long-job cases,
instead of imposing waiting or turnaround limits. Final-source measurements
match the comparison runs; the original simulator entry point reproduces seed 0.

Run `make -C src test` on Linux or use the README's Docker recipe.
[All measurements and reproduction sources](pure-edp-results.json) include the
two control scheduler snapshots, exact final source, source hashes, sixty runs,
and correctness output. All energy and time values are raw simulator counters.
