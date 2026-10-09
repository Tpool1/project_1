#include "scheduler.hpp"
#include "ready_queue.hpp"
#include "idle_predictor.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <unordered_map>
#include <utility>

#ifndef EEC_SMALL_CORES
#define EEC_SMALL_CORES 4 // Number of small cores (IDs 4-7) enabled for work.
#endif
#ifndef EEC_BIG_CORES
#define EEC_BIG_CORES 4 // Number of big cores (IDs 0-3) enabled for work.
#endif
#ifndef EEC_PSTATE
#define EEC_PSTATE P3
#endif
#ifndef EEC_PREEMPTION
#define EEC_PREEMPTION 0 // Experimental; measured gain was below 0.1%.
#endif
#ifndef EEC_PREEMPTION_QUIET
#define EEC_PREEMPTION_QUIET 2000000
#endif
#ifndef EEC_IDLE_PREDICTOR
#define EEC_IDLE_PREDICTOR 0
#endif
#ifndef EEC_EDP_PLACEMENT
#define EEC_EDP_PLACEMENT 1
#endif
#ifndef EEC_DISPATCH_POLICY
#define EEC_DISPATCH_POLICY 2 // 0: reserved, 1: EDP, 2: LJF, 3: SJF, 4: FIFO.
#endif
#ifndef EEC_FINAL_DVFS
#define EEC_FINAL_DVFS 0 // Experimental; measured gain was below 0.1%.
#endif
#ifndef EEC_FINAL_DVFS_QUIET
#define EEC_FINAL_DVFS_QUIET 2000000
#endif
#ifndef EEC_TAIL_P4
#define EEC_TAIL_P4 1
#endif
#ifndef EEC_SMALL_FIRST
#define EEC_SMALL_FIRST 1
#endif

static_assert(EEC_SMALL_CORES >= 0 && EEC_SMALL_CORES <= 4, "Invalid small-core count");
static_assert(EEC_BIG_CORES >= 0 && EEC_BIG_CORES <= 4, "Invalid big-core count");
static_assert(EEC_SMALL_CORES + EEC_BIG_CORES > 0, "At least one core is needed");
static_assert(EEC_PSTATE >= P0 && EEC_PSTATE <= P4, "Invalid P-state");
static_assert(EEC_PREEMPTION == 0 || EEC_PREEMPTION == 1,
              "EEC_PREEMPTION must be zero or one");
static_assert(EEC_PREEMPTION_QUIET >= 0, "Invalid preemption quiet period");
static_assert(EEC_IDLE_PREDICTOR == 0 || EEC_IDLE_PREDICTOR == 1,
              "EEC_IDLE_PREDICTOR must be zero or one");
static_assert(EEC_EDP_PLACEMENT == 0 || EEC_EDP_PLACEMENT == 1,
              "EEC_EDP_PLACEMENT must be zero or one");
static_assert(EEC_DISPATCH_POLICY >= 0 && EEC_DISPATCH_POLICY <= 4,
              "EEC_DISPATCH_POLICY must be between zero and four");
static_assert(EEC_FINAL_DVFS == 0 || EEC_FINAL_DVFS == 1,
              "EEC_FINAL_DVFS must be zero or one");
static_assert(EEC_FINAL_DVFS_QUIET >= 0, "Invalid final-DVFS quiet period");
static_assert(EEC_TAIL_P4 == 0 || EEC_TAIL_P4 == 1,
              "EEC_TAIL_P4 must be zero or one");
static_assert(EEC_SMALL_FIRST == 0 || EEC_SMALL_FIRST == 1,
              "EEC_SMALL_FIRST must be zero or one");

namespace {
enum class CoreStatus { Ready, Running, Sleeping, Waking, Deepening };

struct CoreRecord {
    CoreStatus status = CoreStatus::Ready;
    ProcessId_t pid = 0;
    PState_t pstate = P0;
    bool enabled = false;
    CState_t c_state = C1;
    CState_t target_state = C1;
    Time_t wake_complete_at = 0;
    IdlePredictor idle;
};

struct ProcessRecord {
    Time_t arrival;
    bool dispatched = false;
    std::optional<CPUId_t> core;
};

std::array<CoreRecord, 8> cores;
std::array<ReadyQueue, 8> ready;
std::array<long double, 8> queued_duration{};
std::array<long double, 8> queued_energy{};
std::unordered_map<ProcessId_t, ProcessRecord> processes;
bool initialized = false;
std::uint64_t created = 0;
std::uint64_t completed = 0;
std::uint64_t initial_work = 0;
std::uint64_t wake_requests = 0;
std::uint64_t wake_completions = 0;
std::uint64_t tail_changes = 0;
std::uint64_t final_dvfs_changes = 0;
std::uint64_t preemptions = 0;
std::uint64_t accelerations = 0;
std::uint64_t consolidations = 0;
std::uint64_t big_placements = 0;
std::uint64_t small_placements = 0;
std::uint64_t queue_steals = 0;
Time_t first_preemption = -1;
Time_t last_preemption = -1;
Time_t last_arrival = -1;
Time_t total_wait = 0;
Time_t maximum_wait = 0;

enum class PreemptionKind { Accelerate, Consolidate };
struct Preemption {
    PreemptionKind kind;
    CPUId_t source;
    CPUId_t destination;
    ProcessId_t pid;
};

struct Projection {
    long double energy;
    long double finish;

    long double EDP(long double energy_so_far, Time_t now) const {
        return (energy_so_far + energy) * (static_cast<long double>(now) + finish);
    }
};

struct ReservationChange {
    std::optional<CPUId_t> source;
    CPUId_t destination;
    ProcessId_t pid;
};

long double IdlePower(CPUId_t core, CState_t state) {
    constexpr std::array<long double, 8> big_power = {
        0.0L, 9.6L, 9.6L, 4.0L, 1.6L, 0.0L, 0.0L, 0.0L
    };
    return big_power[state] * (core < 4 ? 1.0L : 0.5L);
}

bool ReadyQueuesEmpty() {
    for (CPUId_t core = 0; core < ready.size(); ++core)
        if (!ready[core].Empty())
            return false;
    return true;
}

CPUId_t CoreAt(unsigned position) {
    return EEC_SMALL_FIRST ? (position + 4) % cores.size() : position;
}

// Input: A core ID and the current time.
// Output: None.
// Side-effects: Requests the predicted idle state for an eligible core.
void SleepCore(CPUId_t core, Time_t now) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Ready && record.status != CoreStatus::Sleeping)
        return;
    if (record.enabled && !ready[core].Empty())
        return;
    if (!record.enabled) {
        if (record.c_state != C6) {
            record.target_state = C6;
            record.status = record.c_state == C3 || record.c_state == C4
                                ? CoreStatus::Deepening : CoreStatus::Sleeping;
            SetCState(core, C6);
            if (record.status == CoreStatus::Sleeping)
                record.c_state = C6;
        }
        return;
    }

    // C6 has zero residence power in the supplied simulator. The optional
    // predictor remains available for controlled comparisons, but immediate
    // C6 is the EDP-minimizing default measured across generator seeds.
    const CState_t next = EEC_IDLE_PREDICTOR ? record.idle.Select(now) : C6;
    if (next <= record.c_state)
        return;
    record.target_state = next;
    record.status = record.c_state == C3 || record.c_state == C4
                        ? CoreStatus::Deepening : CoreStatus::Sleeping;
    SetCState(core, next);
    if (record.status == CoreStatus::Sleeping)
        record.c_state = next;
}

// Input: None.
// Output: None.
// Side-effects: On the first call, records which cores are enabled and
// initializes their ownership and idle timestamps.
void Initialize() {
    if (initialized)
        return;
    initialized = true;
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        cores[core].pid = InvalidProcessId();
        cores[core].idle.Begin(Now());
        cores[core].enabled = static_cast<int>(core) < EEC_BIG_CORES ||
                              (core >= 4 && core < 4 + EEC_SMALL_CORES);
    }
}

// Input: A core ID and the process's remaining work.
// Output: The P-state to use for that work.
// Side-effects: None.
PState_t StateForWork(CPUId_t core, Time_t remaining,
                      PState_t sustained_state = EEC_PSTATE) {
    // P3 minimizes sustained-work energy in this simulator. Use P4 only when
    // it can finish the job in the current interval; faster states save less
    // than one quantum and cost more energy than that local delay can justify.
    const Time_t p4_capacity = core < 4 ? 200 : 120;
    if (EEC_TAIL_P4 && remaining > 0 && remaining <= p4_capacity)
        return P4;
    return sustained_state;
}

struct JobProjection {
    long double duration;
    long double energy;
};

long double StatePower(CPUId_t core, PState_t state) {
    constexpr std::array<long double, 5> big_power = {22.0L, 16.0L, 10.8L, 5.6L, 3.6L};
    return big_power[state] * (core < 4 ? 1.0L : 0.5L);
}

Time_t StateCapacity(CPUId_t core, PState_t state) {
    constexpr std::array<Time_t, 5> big_capacity = {1000, 800, 600, 400, 200};
    return big_capacity[state] * (core < 4 ? 5 : 3) / 5;
}

// Project the simulator's full-quantum execution policy: sustained work uses
// EEC_PSTATE and a final interval uses P4 only when P4 can finish it.
JobProjection ProjectJob(CPUId_t core, Time_t work,
                         PState_t sustained_state = EEC_PSTATE) {
    if (work <= 0)
        return {0, 0};

    const Time_t sustained_capacity = StateCapacity(core, sustained_state);
    const Time_t total_intervals =
        (work + sustained_capacity - 1) / sustained_capacity;
    const Time_t sustained_intervals = total_intervals - 1;
    const Time_t final_work = work - sustained_intervals * sustained_capacity;
    const PState_t final_state = StateForWork(core, final_work, sustained_state);
    const long double intervals = static_cast<long double>(sustained_intervals);
    return {
        static_cast<long double>(total_intervals) * QUANTUM,
        intervals * StatePower(core, sustained_state) * QUANTUM +
            StatePower(core, final_state) * QUANTUM
    };
}

Time_t WakeDelay(const CoreRecord &record, Time_t now) {
    if (record.status == CoreStatus::Waking)
        return std::max<Time_t>(0, record.wake_complete_at - now);
    if (record.status == CoreStatus::Deepening) {
        const Time_t wake = record.target_state == C6 ? 2000000 : 10000;
        return 10000 + wake;
    }
    if (record.status != CoreStatus::Sleeping)
        return 0;
    if (record.c_state == C6)
        return 2000000;
    if (record.c_state == C3 || record.c_state == C4)
        return 10000;
    return 0;
}

// Project all currently admitted work. Reservation changes are evaluated using
// the destination core's duration and energy, so a queue steal cannot retain
// stale accounting from its original core.
Projection ProjectSchedule(
    Time_t now,
    const std::optional<ReservationChange> &reservation_change = std::nullopt,
    const std::optional<std::pair<CPUId_t, PState_t>> &state_change = std::nullopt,
    const std::optional<Preemption> &migration = std::nullopt) {
    std::array<long double, 8> duration = queued_duration;
    std::array<long double, 8> energy = queued_energy;

    if (reservation_change) {
        if (reservation_change->source) {
            const CPUId_t source = *reservation_change->source;
            const JobProjection source_job =
                ProjectJob(source, GetRemaining(reservation_change->pid));
            duration[source] -= source_job.duration;
            energy[source] -= source_job.energy;
        }
        const CPUId_t destination = reservation_change->destination;
        const JobProjection destination_job =
            ProjectJob(destination, GetRemaining(reservation_change->pid));
        duration[destination] += destination_job.duration;
        energy[destination] += destination_job.energy;
    }

    const ProcessId_t invalid = InvalidProcessId();
    std::array<ProcessId_t, 8> running;
    running.fill(invalid);
    for (CPUId_t core = 0; core < cores.size(); ++core)
        if (cores[core].enabled && cores[core].status == CoreStatus::Running)
            running[core] = cores[core].pid;

    if (migration) {
        running[migration->destination] = running[migration->source];
        running[migration->source] = invalid;
    }

    long double finish = 0;
    long double total_energy = 0;
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        if (!cores[core].enabled)
            continue;

        if (running[core] != invalid) {
            PState_t state = cores[core].pstate;
            if (migration && migration->destination == core)
                state = EEC_PSTATE;
            if (state_change && state_change->first == core)
                state = state_change->second;
            const JobProjection running_job =
                ProjectJob(core, GetRemaining(running[core]), state);
            duration[core] += running_job.duration;
            energy[core] += running_job.energy;
        } else if (duration[core] > 0) {
            const Time_t delay = WakeDelay(cores[core], now);
            duration[core] += delay;
            energy[core] += static_cast<long double>(delay) *
                            IdlePower(core, cores[core].c_state);
        }

        finish = std::max(finish, duration[core]);
        total_energy += energy[core];
    }
    return {total_energy, finish};
}

CPUId_t SelectQueue(ProcessId_t pid, Time_t now) {
    CPUId_t best = cores.size();
    long double best_score = std::numeric_limits<long double>::infinity();
    long double best_energy = std::numeric_limits<long double>::infinity();
    const long double energy_so_far = GetTotalEnergyConsumed();
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        if (!cores[core].enabled)
            continue;
        const ReservationChange addition{std::nullopt, core, pid};
        const Projection projected = ProjectSchedule(now, addition);
        const long double score = EEC_EDP_PLACEMENT
                                      ? projected.EDP(energy_so_far, now)
                                      : projected.finish;
        if (score < best_score ||
            (score == best_score && projected.energy < best_energy)) {
            best = core;
            best_score = score;
            best_energy = projected.energy;
        }
    }
    if (best == cores.size())
        ThrowException("No enabled core is available for queue placement");
    return best;
}

void EnqueueForCore(ProcessId_t pid, CPUId_t core) {
    const JobProjection job = ProjectJob(core, GetRemaining(pid));
    ready[core].Enqueue(pid);
    queued_duration[core] += job.duration;
    queued_energy[core] += job.energy;
    if (core < 4)
        ++big_placements;
    else
        ++small_placements;
}

ProcessId_t DequeueForCore(CPUId_t core) {
    const ProcessId_t pid = ready[core].PopNext();
    const JobProjection job = ProjectJob(core, GetRemaining(pid));
    queued_duration[core] -= job.duration;
    queued_energy[core] -= job.energy;
    return pid;
}

ProcessId_t RemoveFromCore(CPUId_t core, ProcessId_t pid) {
    ready[core].Remove(pid);
    const JobProjection job = ProjectJob(core, GetRemaining(pid));
    queued_duration[core] -= job.duration;
    queued_energy[core] -= job.energy;
    return pid;
}

// Select work for an actually ready destination. Every cross-queue dispatch is
// scored using that destination and removed from its original reservation, so
// projected duration and energy cannot silently retain the source core's cost.
std::optional<ProcessId_t> TakeForCore(CPUId_t destination, Time_t now) {
    if (EEC_DISPATCH_POLICY == 0) {
        if (ready[destination].Empty())
            return std::nullopt;
        return DequeueForCore(destination);
    }

    const long double energy_so_far = GetTotalEnergyConsumed();
    const Projection baseline = ProjectSchedule(now);
    long double best_score = std::numeric_limits<long double>::infinity();
    std::optional<CPUId_t> best_source;
    ProcessId_t best_pid = InvalidProcessId();

    for (CPUId_t source = 0; source < cores.size(); ++source) {
        if (ready[source].Empty())
            continue;
        const std::deque<ProcessId_t> &items = ready[source].Items();
        const std::size_t candidates = EEC_DISPATCH_POLICY == 4 ? items.size() : 1;
        for (std::size_t index = 0; index < candidates; ++index) {
            const ProcessId_t pid = EEC_DISPATCH_POLICY == 3
                                        ? items.back()
                                        : items[index];
            long double score = 0;
            if (EEC_DISPATCH_POLICY == 2) {
                score = -static_cast<long double>(GetRemaining(pid));
            } else if (EEC_DISPATCH_POLICY == 3) {
                score = static_cast<long double>(GetRemaining(pid));
            } else if (EEC_DISPATCH_POLICY == 4) {
                score = static_cast<long double>(processes.at(pid).arrival);
            } else if (source == destination) {
                score = baseline.EDP(energy_so_far, now);
            } else {
                const ReservationChange move{source, destination, pid};
                score = ProjectSchedule(now, move).EDP(energy_so_far, now);
            }
            if (score < best_score) {
                best_score = score;
                best_source = source;
                best_pid = pid;
            }
        }
    }

    if (!best_source)
        return std::nullopt;
    const ProcessId_t moved = RemoveFromCore(*best_source, best_pid);
    if (moved != best_pid)
        ThrowException("Reservation changed during dispatch selection");
    if (*best_source != destination)
        ++queue_steals;
    return moved;
}

// Input: A process ID, a core ID, and the current time.
// Output: None.
// Side-effects: Records the first dispatch wait, starts the process on the
// core, and updates their state. Throws if the process or core is unavailable.
void Dispatch(ProcessId_t pid, CPUId_t core, Time_t now, bool ended_idle_period = true) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Ready || record.pid != InvalidProcessId())
        ThrowException("Dispatch requires an unowned core in C1");

    auto process_entry = processes.find(pid);
    if (process_entry == processes.end())
        ThrowException("Dispatch received an unknown process");
    ProcessRecord &process_record = process_entry->second;

    if (!process_record.dispatched) {
        const Time_t wait = now - process_record.arrival;
        total_wait += wait;
        if (wait > maximum_wait)
            maximum_wait = wait;
        process_record.dispatched = true;
    }

    const PState_t state = StateForWork(core, GetRemaining(pid));

    LoadContext(pid, core);
    RunCore(core);
    // RunCore resets the execution timestamp. No work is double-counted
    // when SetPState is applied immediately afterward at the same time.

    SetPState(core, state);
    if (ended_idle_period)
        record.idle.End(now);
    record.pid = pid;
    record.pstate = state;
    record.status = CoreStatus::Running;
    record.c_state = C0;
    process_record.core = core;
}

// Input: A running core.
// Output: The process removed from that core.
// Side-effects: Saves the context and synchronizes scheduler ownership state.
ProcessId_t StopForPreemption(CPUId_t core) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Running || record.pid == InvalidProcessId())
        ThrowException("Preemption requires a running core");
    const ProcessId_t pid = record.pid;
    SaveContext(pid, core);
    processes.at(pid).core.reset();
    record.pid = InvalidProcessId();
    record.status = CoreStatus::Ready;
    record.c_state = C1;
    return pid;
}

// Input: An admitted placement change and the current time.
// Output: None.
// Side-effects: Moves a running job onto an idle core and updates idle history.
void ApplyPreemption(const Preemption &change, Time_t now) {
    if (cores[change.destination].status != CoreStatus::Ready)
        ThrowException("Preemption destination is not ready");
    const ProcessId_t pid = StopForPreemption(change.source);
    if (pid != change.pid)
        ThrowException("Preemption source changed before migration");
    cores[change.source].idle.Begin(now);
    Dispatch(pid, change.destination, now);

    if (change.kind == PreemptionKind::Accelerate)
        ++accelerations;
    else
        ++consolidations;
    if (first_preemption < 0)
        first_preemption = now;
    last_preemption = now;
    ++preemptions;
}

// Input: The current time.
// Output: None.
// Side-effects: Applies at most one EDP-improving heterogeneous placement.
void MaybePreempt(Time_t now) {
    if (!EEC_PREEMPTION || EEC_BIG_CORES == 0 || EEC_SMALL_CORES == 0)
        return;

    // A decision made during an active arrival stream cannot predict the final
    // tail. Wait through a long quiet period before changing placement. This
    // turns preemption into tail rebalancing instead of repeatedly reshuffling
    // a growing workload.
    const Time_t quiet_period = last_arrival < 0 ? 0 : now - last_arrival;
    if (quiet_period < EEC_PREEMPTION_QUIET)
        return;

    // Only migrate onto an already-idle core with no queued work. This avoids
    // wake costs and makes the remaining-work comparison self-contained.
    if (!ReadyQueuesEmpty())
        return;

    const Projection baseline = ProjectSchedule(now);
    const long double energy_so_far = GetTotalEnergyConsumed();
    const long double baseline_edp = baseline.EDP(energy_so_far, now);
    std::optional<Preemption> best;
    long double best_edp = baseline_edp;

    // If a big core finishes first, use it to accelerate a remaining
    // small-core tail. Two quanta of required improvement cover the simulator's
    // timer and tail-P-state quantization.
    for (CPUId_t big = 0; big < 4; ++big) {
        if (!cores[big].enabled || cores[big].status != CoreStatus::Ready)
            continue;
        for (CPUId_t small = 4; small < cores.size(); ++small) {
            if (!cores[small].enabled || cores[small].status != CoreStatus::Running ||
                GetRemaining(cores[small].pid) <= 0)
                continue;
            const Preemption candidate{PreemptionKind::Accelerate, small, big,
                                       cores[small].pid};
            const Projection changed =
                ProjectSchedule(now, std::nullopt, std::nullopt, candidate);
            const long double changed_edp = changed.EDP(energy_so_far, now);
            if (changed.finish + 2 * QUANTUM <= baseline.finish && changed_edp < best_edp) {
                best = candidate;
                best_edp = changed_edp;
            }
        }
    }

    // Conversely, use an idle small core to save energy on a non-critical big-
    // core job, but only when neither projected completion time nor energy grows.
    for (CPUId_t small = 4; small < cores.size(); ++small) {
        if (!cores[small].enabled || cores[small].status != CoreStatus::Ready)
            continue;
        for (CPUId_t big = 0; big < 4; ++big) {
            if (!cores[big].enabled || cores[big].status != CoreStatus::Running ||
                GetRemaining(cores[big].pid) <= 0)
                continue;
            const Preemption candidate{PreemptionKind::Consolidate, big, small,
                                       cores[big].pid};
            const Projection changed =
                ProjectSchedule(now, std::nullopt, std::nullopt, candidate);
            const long double changed_edp = changed.EDP(energy_so_far, now);
            if (changed.finish <= baseline.finish && changed.energy < baseline.energy &&
                changed_edp < best_edp) {
                best = candidate;
                best_edp = changed_edp;
            }
        }
    }

    if (best)
        ApplyPreemption(*best, now);
}

// Apply a P-state change without letting SetPState account the just-finished
// timer interval a second time.
void ChangeRunningState(CPUId_t core, PState_t state) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Running || record.pid == InvalidProcessId())
        ThrowException("P-state change requires a running core");
    SaveContext(record.pid, core);
    LoadContext(record.pid, core);
    RunCore(core);
    SetPState(core, state);
    record.pstate = state;
}

// During the observed final drain, greedily apply one whole-system EDP-
// improving P-state change per timer. Replanning each quantum lets later
// changes account for energy already spent and the current longest tail.
bool FinalDVFSEligible(Time_t now) {
    return EEC_FINAL_DVFS && last_arrival >= 0 &&
           now - last_arrival >= EEC_FINAL_DVFS_QUIET && ReadyQueuesEmpty();
}

void MaybeAdjustFinalDVFS(Time_t now) {
    if (!FinalDVFSEligible(now))
        return;

    const long double energy_so_far = GetTotalEnergyConsumed();
    const Projection baseline = ProjectSchedule(now);
    long double best_edp = baseline.EDP(energy_so_far, now);
    std::optional<CPUId_t> best_core;
    PState_t best_state = EEC_PSTATE;

    for (CPUId_t core = 0; core < cores.size(); ++core) {
        const CoreRecord &record = cores[core];
        if (!record.enabled || record.status != CoreStatus::Running ||
            GetRemaining(record.pid) <= 0)
            continue;
        for (int raw_state = P0; raw_state <= P3; ++raw_state) {
            const PState_t state = static_cast<PState_t>(raw_state);
            if (state == record.pstate)
                continue;
            const Projection changed = ProjectSchedule(
                now, std::nullopt, std::make_pair(core, state));
            const long double changed_edp = changed.EDP(energy_so_far, now);
            if (changed_edp < best_edp) {
                best_edp = changed_edp;
                best_core = core;
                best_state = state;
            }
        }
    }

    if (best_core) {
        ChangeRunningState(*best_core, best_state);
        ++final_dvfs_changes;
    }
}

// Input: The current time.
// Output: None.
// Side-effects: Dispatches queued processes, requests core wakes when needed,
// and moves idle cores toward sleep.
void ScheduleReadyWork(Time_t now) {
    for (unsigned position = 0; position < cores.size(); ++position) {
        const CPUId_t core = CoreAt(position);
        CoreRecord &record = cores[core];
        if (!record.enabled || record.status != CoreStatus::Ready)
            continue;
        const std::optional<ProcessId_t> pid = TakeForCore(core, now);
        if (pid)
            Dispatch(*pid, core, now);
    }

    MaybePreempt(now);

    // Placement already compared the global EDP with this core's wake delay.
    // A nonempty reservation therefore authorizes exactly this core's wake;
    // empty cores are never woken merely because some other queue is long.
    for (unsigned position = 0; position < cores.size(); ++position) {
        const CPUId_t core = CoreAt(position);
        CoreRecord &record = cores[core];
        if (!record.enabled || record.status != CoreStatus::Sleeping ||
            ready[core].Empty())
            continue;
        const Time_t delay = WakeDelay(record, now);
        record.idle.End(now);
        record.status = CoreStatus::Waking;
        record.wake_complete_at = now + delay;
        ++wake_requests;
        SetCState(core, C1);
    }

    for (CPUId_t core = 0; core < cores.size(); ++core)
        SleepCore(core, now);
}
} // namespace

// Input: A new process ID.
// Output: None.
// Side-effects: Records and queues the process, updates creation totals, and
// schedules work. Throws for invalid or duplicate creation.
void CreateProcess(ProcessId_t pid) {
    Initialize();
    const Time_t work = GetRemaining(pid);
    const Time_t now = Now();
    if (work < 0 || !processes.emplace(pid, ProcessRecord{now, false, std::nullopt}).second)
        ThrowException("Invalid or duplicate process creation");
    ++created;
    initial_work += static_cast<std::uint64_t>(work);
    last_arrival = now;
    EnqueueForCore(pid, SelectQueue(pid, now));
    ScheduleReadyWork(now);
}

// Input: A completed process ID.
// Output: None.
// Side-effects: Removes the process, frees its core, updates the completion
// count, and schedules work. Throws if completion does not match a running process.
void ExitProcess(ProcessId_t pid) {
    auto process = processes.find(pid);
    if (process == processes.end() || !process->second.core)
        ThrowException("Completion does not match a running process");
    const CPUId_t owner = *process->second.core;
    if (owner >= cores.size() || cores[owner].status != CoreStatus::Running ||
        cores[owner].pid != pid || GetRemaining(pid) != 0)
        ThrowException("Completion does not match a running process");
    processes.erase(process);

    // BeforeScheduler has already detached this completed PID and set C1.
    // SaveContext here would incorrectly require a still-running C0 core.
    cores[owner].pid = InvalidProcessId();
    cores[owner].status = CoreStatus::Ready;
    cores[owner].c_state = C1;
    cores[owner].idle.Begin(Now());
    ++completed;
    ScheduleReadyWork(Now());
}

// Input: The current time.
// Output: None.
// Side-effects: Adjusts running cores' P-states when needed and schedules
// ready work.
void TimerInterrupt(Time_t now) {
    Initialize();
    const bool final_dvfs = FinalDVFSEligible(now);
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        CoreRecord &record = cores[core];
        if (record.status != CoreStatus::Running)
            continue;
        const PState_t sustained_state = final_dvfs ? record.pstate : EEC_PSTATE;
        const PState_t state =
            StateForWork(core, GetRemaining(record.pid), sustained_state);
        if (state != record.pstate) {
            ChangeRunningState(core, state);
            ++tail_changes;
        }
    }
    MaybeAdjustFinalDVFS(now);
    ScheduleReadyWork(now);
}

// Input: The ID of the core whose sleep or wake transition finished.
// Output: None.
// Side-effects: Updates the core state, counts completed wakes, and schedules
// work. Throws if the transition completion was unexpected.
void CStateTransitionComplete(CPUId_t core) {
    if (core < cores.size() && cores[core].status == CoreStatus::Deepening) {
        cores[core].status = CoreStatus::Sleeping;
        cores[core].c_state = cores[core].target_state;
        ScheduleReadyWork(Now());
        return;
    }
    if (core >= cores.size() || cores[core].status != CoreStatus::Waking)
        ThrowException("Unexpected C-state transition completion");
    cores[core].status = CoreStatus::Ready;
    cores[core].c_state = C1;
    cores[core].wake_complete_at = 0;
    ++wake_completions;
    // Other running cores may not have reached BeforeScheduler at this timestamp.
    ScheduleReadyWork(Now());
}

// Input: The final simulation time.
// Output: None.
// Side-effects: Checks that all work is done and prints run statistics. Throws
// if work or core ownership remains.
void SimulationComplete(Time_t now) {
    if (!ReadyQueuesEmpty() || !processes.empty() || created != completed)
        ThrowException("Simulation stopped with unfinished scheduler work");
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        const CoreRecord &record = cores[core];
        if (record.status == CoreStatus::Running || record.pid != InvalidProcessId())
            ThrowException("Simulation stopped with an owned CPU context");
        if (std::abs(queued_duration[core]) > 0.01L ||
            std::abs(queued_energy[core]) > 0.01L)
            ThrowException("Simulation stopped with nonzero queued projections");
    }

    const double energy = GetTotalEnergyConsumed();
    const double edp = energy * static_cast<double>(now);
    const double lower_bound = static_cast<double>(initial_work) * (35.0 / 3.0);
    std::cout << std::setprecision(12)
              << "Run stopped at " << FormatTime(now) << " after consuming "
              << energy / 3600000000.0 << " kWh\n"
              << "Raw time: " << now << "; raw energy: " << energy
              << "; completed: " << completed << '/' << created << '\n'
              << "EDP (raw energy × raw time): " << edp << '\n'
              << "Mean/max dispatch wait (raw time): "
              << (created ? static_cast<double>(total_wait) / created : 0.0)
              << '/' << maximum_wait << "; wakes: " << wake_completions << '/' << wake_requests
              << "; tail P-state changes: " << tail_changes
              << "; final-DVFS changes: " << final_dvfs_changes
              << "; preemptions (accelerate/consolidate): " << preemptions
              << " (" << accelerations << '/' << consolidations << ")"
              << "; first/last: "
              << first_preemption << '/' << last_preemption
              << "; last arrival: " << last_arrival
              << "; reservations (big/small): " << big_placements
              << '/' << small_placements
              << "; queue steals: " << queue_steals << '\n'
              << "Active-energy lower bound: " << lower_bound
              << "; overhead: " << (lower_bound ? 100.0 * (energy / lower_bound - 1.0) : 0.0)
              << "%\n";
}
