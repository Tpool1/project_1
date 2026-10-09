#include "scheduler.hpp"
#include "ready_queue.hpp"
#include "idle_predictor.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <unordered_map>

#ifndef EEC_SMALL_CORES
#define EEC_SMALL_CORES 4 // Number of small cores (IDs 4-7) enabled for work.
#endif
#ifndef EEC_BIG_CORES
#define EEC_BIG_CORES 3 // Number of big cores (IDs 0-2) enabled for work.
#endif
#ifndef EEC_PSTATE
#define EEC_PSTATE P3
#endif
#ifndef EEC_PREEMPTION
#define EEC_PREEMPTION 1
#endif
#ifndef EEC_PREEMPTION_QUIET
#define EEC_PREEMPTION_QUIET 2000000
#endif

static_assert(EEC_SMALL_CORES >= 0 && EEC_SMALL_CORES <= 4, "Invalid small-core count");
static_assert(EEC_BIG_CORES >= 0 && EEC_BIG_CORES <= 4, "Invalid big-core count");
static_assert(EEC_SMALL_CORES + EEC_BIG_CORES > 0, "At least one core is needed");
static_assert(EEC_PSTATE >= P0 && EEC_PSTATE <= P4, "Invalid P-state");
static_assert(EEC_PREEMPTION == 0 || EEC_PREEMPTION == 1,
              "EEC_PREEMPTION must be zero or one");
static_assert(EEC_PREEMPTION_QUIET >= 0, "Invalid preemption quiet period");

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

long double IdlePower(CPUId_t core) {
    // C4 power. It is a conservative middle ground for the predictor's C1/C4/C6
    // choices and is used equally by both counterfactual schedules.
    return core < 4 ? 1.6L : 0.8L;
}

bool ReadyQueuesEmpty() {
    for (CPUId_t core = 0; core < ready.size(); ++core)
        if (!ready[core].Empty())
            return false;
    return true;
}

std::size_t ReadyQueueSize() {
    std::size_t size = 0;
    for (const ReadyQueue &queue : ready)
        size += queue.Size();
    return size;
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

    // shortcut: no future-arrival getter exists; pass one here if the simulator exposes it.
    const CState_t next = record.idle.Select(now);
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
PState_t StateForWork(CPUId_t core, Time_t remaining) {
    // P3 minimizes sustained-work energy in this simulator. Use P4 only when
    // it can finish the job in the current interval; faster states save less
    // than one quantum and cost more energy than that local delay can justify.
    const Time_t p4_capacity = core < 4 ? 200 : 120;
    if (remaining > 0 && remaining <= p4_capacity)
        return P4;
    return EEC_PSTATE;
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
JobProjection ProjectJob(CPUId_t core, Time_t work) {
    if (work <= 0)
        return {0, 0};

    const Time_t sustained_capacity = StateCapacity(core, EEC_PSTATE);
    const Time_t total_intervals =
        (work + sustained_capacity - 1) / sustained_capacity;
    const Time_t sustained_intervals = total_intervals - 1;
    const Time_t final_work = work - sustained_intervals * sustained_capacity;
    const PState_t final_state = StateForWork(core, final_work);
    const long double intervals = static_cast<long double>(sustained_intervals);
    return {
        static_cast<long double>(total_intervals) * QUANTUM,
        intervals * StatePower(core, EEC_PSTATE) * QUANTUM +
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

long double ProjectedCoreFinish(ProcessId_t pid, CPUId_t core, Time_t now) {
    long double finish = queued_duration[core];
    if (cores[core].status == CoreStatus::Running) {
        finish += ProjectJob(core, GetRemaining(cores[core].pid)).duration;
    } else if (!ready[core].Empty() || cores[core].status != CoreStatus::Ready) {
        finish += WakeDelay(cores[core], now);
    }
    return finish + ProjectJob(core, GetRemaining(pid)).duration;
}

CPUId_t SelectQueue(ProcessId_t pid, Time_t now) {
    CPUId_t best = cores.size();
    long double best_finish = std::numeric_limits<long double>::infinity();
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        if (!cores[core].enabled)
            continue;
        const long double finish = ProjectedCoreFinish(pid, core, now);
        if (finish < best_finish) {
            best = core;
            best_finish = finish;
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

// Preserve global LJF at dispatch time while retaining per-core reservations.
// A ready core may correct an obsolete arrival-time projection by taking the
// longest head job from another core's queue.
ProcessId_t TakeGlobalLongest(CPUId_t destination) {
    std::optional<CPUId_t> source;
    Time_t longest = -1;
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        if (ready[core].Empty())
            continue;
        const Time_t remaining = GetRemaining(ready[core].Items().front());
        if (!source || remaining > longest) {
            source = core;
            longest = remaining;
        }
    }
    if (!source)
        ThrowException("No queued process is available for dispatch");
    if (*source != destination)
        ++queue_steals;
    return DequeueForCore(*source);
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

// Input: An optional placement change for the current runnable workload.
// Output: Projected energy from now until completion and projected duration.
// Side-effects: None. Future arrivals are deliberately excluded.
Projection ProjectRemainingWork(const std::optional<Preemption> &change) {
    const ProcessId_t invalid = InvalidProcessId();
    std::array<ProcessId_t, 8> running;
    running.fill(invalid);
    for (CPUId_t core = 0; core < cores.size(); ++core)
        if (cores[core].enabled && cores[core].status == CoreStatus::Running)
            running[core] = cores[core].pid;

    if (change) {
        running[change->destination] = running[change->source];
        running[change->source] = invalid;
    }

    std::array<long double, 8> available{};
    std::array<long double, 8> active_energy{};
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        if (!cores[core].enabled || running[core] == invalid)
            continue;
        const JobProjection job = ProjectJob(core, GetRemaining(running[core]));
        available[core] = job.duration;
        active_energy[core] = job.energy;
    }

    long double finish = 0;
    for (CPUId_t core = 0; core < cores.size(); ++core)
        if (cores[core].enabled)
            finish = std::max(finish, available[core]);

    long double energy = 0;
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        if (!cores[core].enabled)
            continue;
        energy += active_energy[core];
        energy += (finish - available[core]) * IdlePower(core);
    }
    return {energy, finish};
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

    const Projection baseline = ProjectRemainingWork(std::nullopt);
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
            const Projection changed = ProjectRemainingWork(candidate);
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
            const Projection changed = ProjectRemainingWork(candidate);
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

// Input: The current time.
// Output: None.
// Side-effects: Dispatches queued processes, requests core wakes when needed,
// and moves idle cores toward sleep.
void ScheduleReadyWork(Time_t now) {
    for (unsigned position = 0; position < cores.size(); ++position) {
        const CPUId_t core = (position + 4) % cores.size();
        CoreRecord &record = cores[core];
        if (record.enabled && record.status == CoreStatus::Ready && !ReadyQueuesEmpty())
            Dispatch(TakeGlobalLongest(core), core, now);
    }

    MaybePreempt(now);

    std::size_t waking = 0;
    for (const CoreRecord &record : cores)
        if (record.status == CoreStatus::Waking)
            ++waking;

    // Reservations influence projected load, while wake admission remains
    // work-conserving: request enough capacity for the total queued demand.
    for (CState_t state : {C4, C6}) {
        for (unsigned position = 0;
             position < cores.size() && waking < ReadyQueueSize(); ++position) {
            const CPUId_t core = (position + 4) % cores.size();
            CoreRecord &record = cores[core];
            if (!record.enabled || record.status != CoreStatus::Sleeping ||
                record.c_state != state)
                continue;
            record.idle.End(now);
            record.status = CoreStatus::Waking;
            record.wake_complete_at = now + (state == C6 ? 2000000 : 10000);
            ++wake_requests;
            ++waking;
            SetCState(core, C1);
        }
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
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        CoreRecord &record = cores[core];
        if (record.status != CoreStatus::Running)
            continue;
        const PState_t state = StateForWork(core, GetRemaining(record.pid));
        if (state != record.pstate) {
            // Reset the run timestamp so SetPState won't account this quantum twice.
            SaveContext(record.pid, core);
            LoadContext(record.pid, core);
            RunCore(core);
            SetPState(core, state);
            record.pstate = state;
            ++tail_changes;
        }
    }
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
