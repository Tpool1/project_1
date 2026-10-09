#include "scheduler.hpp"
#include "ready_queue.hpp"
#include "idle_predictor.hpp"

#include <array>
#include <iomanip>
#include <unordered_map>

#ifndef EEC_SMALL_CORES
#define EEC_SMALL_CORES 4 // Number of small cores (IDs 4-7) enabled for work.
#endif
#ifndef EEC_BIG_CORES
#define EEC_BIG_CORES 4 // Number of big cores (IDs 0-3) enabled for work.
#endif
#ifndef EEC_PSTATE
#define EEC_PSTATE P3
#endif

/*
 *
 * TODO: preemption policy, explicity compare edp cost of scheduling on low vs high core
 */

static_assert(EEC_SMALL_CORES >= 0 && EEC_SMALL_CORES <= 4, "Invalid small-core count");
static_assert(EEC_BIG_CORES >= 0 && EEC_BIG_CORES <= 4, "Invalid big-core count");
static_assert(EEC_SMALL_CORES + EEC_BIG_CORES > 0, "At least one core is needed");
static_assert(EEC_PSTATE >= P0 && EEC_PSTATE <= P4, "Invalid P-state");

namespace {
enum class CoreStatus { Ready, Running, Sleeping, Waking, Deepening };

struct CoreRecord {
    CoreStatus status = CoreStatus::Ready;
    ProcessId_t pid = 0;
    PState_t pstate = P0;
    bool enabled = false;
    CState_t c_state = C1;
    CState_t target_state = C1;
    IdlePredictor idle;
};

struct ProcessRecord {
    Time_t arrival;
    bool dispatched = false;
    std::optional<CPUId_t> core;
};

std::array<CoreRecord, 8> cores;
ReadyQueue ready;
std::unordered_map<ProcessId_t, ProcessRecord> processes;
bool initialized = false;
std::uint64_t created = 0;
std::uint64_t completed = 0;
std::uint64_t initial_work = 0;
std::uint64_t wake_requests = 0;
std::uint64_t wake_completions = 0;
std::uint64_t tail_changes = 0;
Time_t total_wait = 0;
Time_t maximum_wait = 0;

// Input: A core ID and the current time.
// Output: None.
// Side-effects: Requests the predicted idle state for an eligible core.
void SleepCore(CPUId_t core, Time_t now) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Ready && record.status != CoreStatus::Sleeping)
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
    // libsim's big-core rates for P0 through P4 are 1000, 800, 600, 400,
    // and 200 work units per quantum. Small cores run at 60% of those rates.
    // Choose the slowest state that can finish the remaining work this quantum.
    constexpr std::array<Time_t, 5> big_work_per_quantum = {1000, 800, 600, 400, 200};
    if (remaining > 0) {
        for (int state = P4; state >= P0; --state) {
            const Time_t capacity = big_work_per_quantum[state] * (core >= 4 ? 3 : 5) / 5;
            if (remaining <= capacity)
                return static_cast<PState_t>(state);
        }
    }
    return EEC_PSTATE;
}

// Input: A process ID, a core ID, and the current time.
// Output: None.
// Side-effects: Records the first dispatch wait, starts the process on the
// core, and updates their state. Throws if the process or core is unavailable.
void Dispatch(ProcessId_t pid, CPUId_t core, Time_t now) {
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
    record.idle.End(now);
    record.pid = pid;
    record.pstate = state;
    record.status = CoreStatus::Running;
    record.c_state = C0;
    process_record.core = core;
}

// Input: The current time.
// Output: None.
// Side-effects: Dispatches queued processes, requests core wakes when needed,
// and moves idle cores toward sleep.
void ScheduleReadyWork(Time_t now) {
    for (unsigned position = 0; position < cores.size(); ++position) {
        const CPUId_t core = (position) % cores.size();
        CoreRecord &record = cores[core];
        if (!ready.Empty() && record.enabled && record.status == CoreStatus::Ready) {
            std::optional<ProcessId_t> pid = ready.PopNext();
            if (pid)
                Dispatch(*pid, core, now);
        }
    }

    std::size_t waking = 0;
    for (const CoreRecord &record : cores)
        if (record.status == CoreStatus::Waking)
            ++waking;

    // Prefer shallower sleepers; count an in-flight wake as future capacity.
    for (CState_t state : {C4, C6}) {
        for (unsigned position = 0; position < cores.size() && waking < ready.Size(); ++position) {
            const CPUId_t core = (position + 4) % cores.size();
            CoreRecord &record = cores[core];
            if (record.enabled && record.status == CoreStatus::Sleeping &&
                record.c_state == state) {
                record.status = CoreStatus::Waking;
                ++wake_requests;
                SetCState(core, C1);
                ++waking;
            }
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
    ready.Enqueue(pid);
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
    ++wake_completions;
    // Other running cores may not have reached BeforeScheduler at this timestamp.
    ScheduleReadyWork(Now());
}

// Input: The final simulation time.
// Output: None.
// Side-effects: Checks that all work is done and prints run statistics. Throws
// if work or core ownership remains.
void SimulationComplete(Time_t now) {
    if (!ready.Empty() || !processes.empty() || created != completed)
        ThrowException("Simulation stopped with unfinished scheduler work");
    for (const CoreRecord &record : cores)
        if (record.status == CoreStatus::Running || record.pid != InvalidProcessId())
            ThrowException("Simulation stopped with an owned CPU context");

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
              << "; tail P-state changes: " << tail_changes << '\n'
              << "Active-energy lower bound: " << lower_bound
              << "; overhead: " << (lower_bound ? 100.0 * (energy / lower_bound - 1.0) : 0.0)
              << "%\n";
}
