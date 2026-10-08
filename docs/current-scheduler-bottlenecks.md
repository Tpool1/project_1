# Current scheduler: energy-delay bottlenecks

Measured October 7, 2026 against the current working tree. The clearest remaining
batch-EDP bottleneck is the disabled fourth big core. The broader limitations are
reactive wake decisions and a fixed frequency policy that does not distinguish
arrival-limited workloads from finite batches. The previous fixed-C4 energy
bottleneck has already been addressed by the current idle timeout.

The current baseline is four small cores, three big cores, P3 with final-interval
P4, nonpreemptive shortest-job-first (SJF), and C4 for 10,000 raw clock units per
idle episode before requesting C6. This differs from the baseline called
“current” in [the earlier EDP report](scheduler-edp-analysis.md).

The scheduler prints **batch EDP = total energy × final simulation clock**.
This is the primary metric here. Energy × mean job turnaround is reported
separately because it ranks policies differently. All measurements use raw
simulator units; its physical-unit labels are inconsistent.

**Evidence.** A fresh sweep ran 22 configurations on generator seeds 0–5 and ten
synthetic workloads: 352 runs. Each run checked completion, unique context
ownership, useful-work preservation across callbacks, and the active-energy
lower bound. Postprocessing checked identical paired workloads, per-core work
and completion totals, and wait + service = turnaround. Ten baseline workloads
exactly reproduced the archived timeout measurements. Production source was
unchanged. [Full results and reproduction sources](current-scheduler-bottleneck-results.json)
record current source hashes and every run.

The following changes are arithmetic means of six paired percentage changes
versus the current baseline. They are measured comparisons, not a proof of a
global optimum.

| Change from current baseline | Energy | Final clock | Batch EDP | Mean turnaround |
| --- | ---: | ---: | ---: | ---: |
| Enable fourth big core; retain 10,000 timeout | +1.17% | −2.74% | **−1.61%** | −33.32% |
| Enable fourth big core; timeout 1 | +1.07% | −2.62% | −1.58% | −25.58% |
| Disable another big core | −1.76% | +4.24% | +2.40% | +59.88% |
| Use only the four small cores | −9.77% | +25.39% | +13.14% | +356.99% |
| Increase timeout to 100,000 | +0.19% | −0.20% | −0.02% | −22.16% |
| Increase timeout to 500,000 | +2.67% | −0.58% | +2.07% | −26.57% |
| Keep enabled idle cores in C4 throughout these runs | +28.42% | −0.72% | +27.50% | −29.62% |
| Fixed P0, without tail P4 | +55.66% | −6.24% | +45.95% | −80.66% |
| Fixed P1, without tail P4 | +44.02% | −6.37% | +34.84% | −79.84% |
| Fixed P2, without tail P4 | +29.20% | −5.61% | +21.95% | −64.54% |
| FIFO instead of SJF | −0.01% | +0.12% | +0.11% | +25.84% |
| Assign largest eligible queued job to a big core | −0.07% | +0.21% | +0.14% | +21.55% |
| Disable final-interval P4 | +0.39% | unchanged | +0.39% | unchanged |

**1. Static core admission leaves useful capacity unavailable.**
[The default](../src/scheduler.cpp) permanently disables core 3, even with a
backlog. P3 peak capacity is 2.16 work units per clock unit with the current
seven cores, versus 2.56 with all eight: an 18.52% capacity increase. Actual
runtime improves less because arrivals and wake delays also constrain it.

Enabling core 3 with the existing timeout improves batch EDP on all six seeds,
by 0.11–2.68%, and gives the best average generator-seed batch EDP among the
22 tested configurations. On seed 0, energy increases from 1,257,322,200 to
1,272,192,400, final clock falls from 90,554,000 to 87,101,000, and EDP falls
from 1.138556×10¹⁷ to 1.108092×10¹⁷. Lower energy alone is insufficient:
removing big cores saves energy but increases the product.

**2. Wake decisions ignore execution capacity already in flight.**
`ScheduleReadyWork` compares the number of waking cores with ready-queue length.
It does not compare queued work with remaining work on running cores, predicted
completion times, or a waking core's time until availability. Wakes begin only
after work queues. C6 takes 2,000,000 raw units to wake, versus 10,000 for C4;
demand during C4-to-C6 deepening must wait for that transition before requesting
a wake. These limits can delay useful capacity or request capacity that existing
cores will provide sooner. The latter is a source-level opportunity, not a
measured count of unnecessary wakes.

On seed 0, mean turnaround is 3,175,930: 3,128,135 of dispatch wait and 47,795
of service. Waiting contributes 98.50%; its six-seed mean share is 98.29%.
Dispatch wait includes both backlog and wake latency; this telemetry does not
separate their individual contributions.

Extending the warm timeout to 100,000 improves mean turnaround by 22.16% on
average with nearly unchanged average batch EDP. However, seed-level batch EDP
changes range from −0.84% to +0.83%, and one seed's mean turnaround increases
8.05%. A longer fixed timeout is therefore not a demonstrated universal fix.
A better next experiment would compare wake availability with expected running
core completion and retain shallow capacity only when that comparison justifies
its energy cost.

**3. Fixed P3 does not optimize EDP across workload phases.**
`StateForWork` selects P4 for a one-quantum remainder and otherwise returns the
compile-time P-state. It does not use backlog, accumulated energy, elapsed
time, or an estimate of the remaining completion horizon. P3 minimizes active
energy per work in this simulator, which does not establish an EDP minimum.

The generated workloads are strongly constrained by their arrival schedule.
Seed 0's last arrival is at 83,610,000 versus final clock 90,554,000. Even
eliminating all time after that arrival could reduce clock by at most 7.67% at
unchanged energy; the last job still needs execution. Faster fixed states pay
for speed throughout the run while gaining relatively little on final clock,
which explains their substantially worse batch EDP.

Finite workloads expose the opposite tradeoff. Fixed P0 reduces batch EDP by
**36.79% on the 32-job burst**, **37.26% on the mixed finite workload**, and
**70.29% on the single-job case**. The burst's energy increases 56.96%, but its
clock falls 59.73%. These are measured evidence of a frequency-policy limitation
across workload types. Phase-aware frequency selection or a carefully limited
final-drain boost deserves evaluation; a generic backlog boost is not established
as beneficial by these results. Online decisions must use observed work and
arrival history, without assuming knowledge of the final arrival.

**4. Queue order concentrates delay on long jobs.**
SJF keeps jobs on their assigned cores until completion. Seed-0 p99 turnaround
is 18,057,000 and maximum turnaround is 23,805,000. FIFO reduces those to
7,394,000 and 7,501,000. Across seeds, FIFO reduces p99 by 59.57% and maximum
turnaround by 68.40%, but increases mean turnaround by 25.84% and average batch
EDP by 0.11%. This is a demonstrated tail-latency limitation, not a major
batch-EDP improvement from changing queue order alone. All jobs complete;
the results do not demonstrate starvation.

The big-core filter accepts jobs with at least 121 work units, while generated
jobs initially contain 9,000–20,000 units. It provides no meaningful size
distinction for that workload. Nevertheless, simply assigning largest jobs to
big cores worsens average batch EDP by 0.14%. More sophisticated placement or
aging would need its own evidence.

**What is already addressed, and what should not be the next priority.**
The current timeout leaves only 888,800 nonstartup idle energy on seed 0,
**0.0707% of total energy**, versus 357,616,000 in the historical fixed-C4
baseline. Across current seeds, this idle share is 0.0636–0.0783%. Active
P3/P4 execution accounts for 1,256,375,800 on seed 0; startup accounts for
57,600. That decomposition uses actual job-to-core assignments and the
simulator's full-interval rates. The old report's approximately 22% idle-energy
opportunity should not be presented as a remaining current bottleneck.

Tail P4 already saves 0.39% of current energy/EDP without changing delay.
Queue insertion and filtered dequeue are linear, but no evidence establishes
their wall-clock overhead as a simulated-EDP bottleneck. Hardware progress and
energy are accounted using simulator time; no wall-clock performance profile
was collected.

For the current generated workloads, enabling the fourth big core is the
strongest demonstrated next improvement. Beyond that, evaluate coupled wake,
core-admission, and phase-sensitive frequency decisions. If the intended delay
term is mean turnaround instead of final clock, prioritize queueing and wake
latency: fixed P1 reduces that alternative product by 70.96% across seeds,
despite worsening batch EDP by 34.84%.
