// Energy-first scheduler for the supplied eight-core simulator.
#include "scheduler.hpp"
#include "ready_queue.hpp"

#include <array>
#include <iomanip>
#include <optional>
#include <unordered_map>

// Compile-time controls allow paired benchmarks without changing the workload.
#ifndef EEC_SMALL_CORES
#define EEC_SMALL_CORES 4 // Number of small cores (IDs 4-7) enabled for work.
#endif
#ifndef EEC_BIG_CORES
#define EEC_BIG_CORES 4 // Number of big cores (IDs 0-3) enabled for work.
#endif
#ifndef EEC_PSTATE
#define EEC_PSTATE P3 // P-state used for normal execution.
#endif
#ifndef EEC_TIMEOUT_QUANTA_2
#define EEC_TIMEOUT_QUANTA_2 3 // C1 residence before entering C2.
#endif
#ifndef EEC_TIMEOUT_QUANTA_3
#define EEC_TIMEOUT_QUANTA_3 5 // C2 residence before entering C3.
#endif
#ifndef EEC_TIMEOUT_QUANTA_4
#define EEC_TIMEOUT_QUANTA_4 5 // C3 residence before entering C4.
#endif
#ifndef EEC_TIMEOUT_QUANTA_6
#define EEC_TIMEOUT_QUANTA_6 10 // C4 residence before entering C6 (C5 is unused).
#endif

static_assert(EEC_SMALL_CORES >= 0 && EEC_SMALL_CORES <= 4, "Invalid small-core count");
static_assert(EEC_BIG_CORES >= 0 && EEC_BIG_CORES <= 4, "Invalid big-core count");
static_assert(EEC_SMALL_CORES + EEC_BIG_CORES > 0, "At least one core is needed");
static_assert(EEC_PSTATE >= P0 && EEC_PSTATE <= P4, "Invalid P-state");
static_assert(EEC_TIMEOUT_QUANTA_2 > 0 && EEC_TIMEOUT_QUANTA_3 > 0 &&
              EEC_TIMEOUT_QUANTA_4 > 0 && EEC_TIMEOUT_QUANTA_6 > 0,
              "Idle timeouts must be positive");

namespace {
enum class CoreStatus { Ready, Running, Sleeping, Waking, Deepening };

struct CoreRecord {
    CoreStatus status = CoreStatus::Ready;
    ProcessId_t pid = 0;
    PState_t pstate = P0;
    bool enabled = false;
    CState_t c_state = C1;
    CState_t target_state = C1;
    Time_t c_state_entered_at = 0;
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

// Inputs: core ID and current time. Output: none. Side effects: after the
// current idle state's timeout, requests the next allowed C-state and updates
// the core record; asynchronous C3->C4 and C4->C6 requests mark it Deepening.
// Disabled cores request C6 immediately. Other core statuses are unchanged.
void SleepCore(CPUId_t core, Time_t now) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Ready && record.status != CoreStatus::Sleeping)
        return;
    if (!record.enabled) {
        if (record.c_state != C6) {
            SetCState(core, C6);
            record.c_state = C6;
            record.status = CoreStatus::Sleeping;
        }
        return;
    }

    CState_t next = record.c_state;
    Time_t timeout = 0;
    switch (record.c_state) {
    case C1: next = C2; timeout = EEC_TIMEOUT_QUANTA_2; break;
    case C2: next = C3; timeout = EEC_TIMEOUT_QUANTA_3; break;
    case C3: next = C4; timeout = EEC_TIMEOUT_QUANTA_4; break;
    case C4: next = C6; timeout = EEC_TIMEOUT_QUANTA_6; break;
    default: return;
    }
    if (now - record.c_state_entered_at < timeout * QUANTUM)
        return;
    record.target_state = next;
    // From C3 or C4, transition completes asynchronously. Core is "deepening" until callback arrives
    record.status = record.c_state == C3 || record.c_state == C4
                        ? CoreStatus::Deepening : CoreStatus::Sleeping;
    SetCState(core, next);
    if (record.status == CoreStatus::Sleeping) {
        record.c_state = next;
        record.c_state_entered_at = now;
    }
}

// Inputs: none. Output: none. Postcondition: core ownership and enabled flags
// are initialized once, leaving cores ready for the first scheduling pass.
void Initialize() {
    if (initialized)
        return;
    initialized = true;
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        cores[core].pid = InvalidProcessId();
        cores[core].c_state_entered_at = Now();
        // enable the core ids according to the simulator
        cores[core].enabled = static_cast<int>(core) < EEC_BIG_CORES ||
                              (core >= 4 && core < 4 + EEC_SMALL_CORES);
        // Constructors establish C1. ScheduleReadyWork dispatches admitted
        // work before putting unused cores into their configured idle states.
    }
}

// Inputs: core ID and remaining work. Output: selected P-state. Postcondition:
// scheduler state is unchanged; a short final interval may select P4.
PState_t StateForWork(CPUId_t core, Time_t remaining) {
    // At a timer interrupt, P4 completes <=120 small-core work units
    // (<=200 on a big core) in one quantum for less energy than P3.
    // These thresholds come from libsim.so: CPU::BeforeScheduler applies
    // speed[P4] * scale[core type] * elapsed, with P4 speed 0.20, big
    // scale 1.0, and small scale 0.60. For QUANTUM=1000,
    // that is 200 work units on a big core and 120 on a small core.
    const Time_t p4_work_per_quantum = core >= 4 ? 120 : 200;
    if (remaining > 0 && remaining <= p4_work_per_quantum)
        return P4;
    return EEC_PSTATE;
}

// Inputs: process ID, ready core ID, and current time. Output: none.
// Postcondition: the process context runs on that core at its selected
// P-state, and first-dispatch wait is recorded when applicable.
void Dispatch(ProcessId_t pid, CPUId_t core, Time_t now) {
    // Verify the target core is ready and not already assigned to a process.
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Ready || record.pid != InvalidProcessId())
        ThrowException("Dispatch requires an unowned core in C1");

    // Look up the process record and reject IDs that were never created.
    auto process_entry = processes.find(pid);
    if (process_entry == processes.end())
        // unable to resolve pid in processes map
        ThrowException("Dispatch received an unknown process");
    ProcessRecord &process_record = process_entry->second;

    // Count the process's ready-queue wait once, on its first dispatch.
    // Keep track of total weight/max weight for metrics printed upon exit
    if (!process_record.dispatched) {
        const Time_t wait = now - process_record.arrival;
        total_wait += wait;
        if (wait > maximum_wait)
            maximum_wait = wait;
        process_record.dispatched = true;
    }

    // Choose a P-state based on the core type and the process's remaining work.
    const PState_t state = StateForWork(core, GetRemaining(pid));

    // Load the saved process context, then start execution on the core.
    LoadContext(pid, core);
    RunCore(core);
    // RunCore resets the execution timestamp. No work is double-counted
    // when SetPState is applied immediately afterward at the same time.

    // Apply the selected P-state and record the core's new ownership and state.
    SetPState(core, state);
    record.pid = pid;
    record.pstate = state;
    record.status = CoreStatus::Running;
    record.c_state = C0;
    process_record.core = core;
}

// Inputs: current time. Output: none. Postcondition: queued work is dispatched to
// available enabled cores; additional required cores are requested awake, and
// unused ready cores are put into their configured idle states.
void ScheduleReadyWork(Time_t now) {
    // Prefer ready small cores before experimental big-core configurations.
    for (unsigned position = 0; position < cores.size(); ++position) {
        // visits cores in this order as position goes from 0 to 7:
        // 4, 5, 6, 7, 0, 1, 2, 3
        // Small cores = 4, 5, 6, 7
        const CPUId_t core = (position + 4) % cores.size();
        CoreRecord &record = cores[core];
        if (!ready.Empty() && record.enabled && record.status == CoreStatus::Sleeping &&
            record.c_state == C2) {
            SetCState(core, C1);
            record.status = CoreStatus::Ready;
            record.c_state = C1;
            record.c_state_entered_at = now;
        }
        if (!ready.Empty() && record.enabled && record.status == CoreStatus::Ready) {
            std::optional<ProcessId_t> pid;
            if (core < 4) {
                // High-perf cores. There's no reason to schedule here if time remaining is <= 120
                // (the amount of work a low-perf core can get done in one time quantum)
                pid = ready.PopNext(121);
            } else {
                pid = ready.PopNext();
            }
            if (pid)
                Dispatch(*pid, core, now);
        }
    }

    // Are there any cores in the process of waking up?
    std::size_t waking = 0;
    for (const CoreRecord &record : cores)
        if (record.status == CoreStatus::Waking)
            ++waking;

    // Prefer shallower sleepers; count an in-flight wake as future capacity.
    for (CState_t state : {C3, C4, C6}) {
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

    // Put unused (ready) cores to sleep
    for (CPUId_t core = 0; core < cores.size(); ++core)
        SleepCore(core, now);
}
} // namespace

// Inputs: newly created process ID. Output: none. Postcondition: the process
// is recorded, counted, queued, and scheduled if capacity is available.
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

// Inputs: completed process ID. Output: none. Postcondition: a validated
// completed process is removed, its core is released, and queued work is
// scheduled; invalid completion state raises an exception.
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
    cores[owner].c_state_entered_at = Now();
    ++completed;
    ScheduleReadyWork(Now());
}

// Inputs: timer timestamp.
// Output: none. Postcondition: eligible running jobs use the appropriate
// final-interval P-state, then ready work is scheduled.
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

// Complete either an idle deepening request or a wake request.
void CStateTransitionComplete(CPUId_t core) {
    if (core < cores.size() && cores[core].status == CoreStatus::Deepening) {
        cores[core].status = CoreStatus::Sleeping;
        cores[core].c_state = cores[core].target_state;
        cores[core].c_state_entered_at = Now();
        ScheduleReadyWork(Now());
        return;
    }
    if (core >= cores.size() || cores[core].status != CoreStatus::Waking)
        ThrowException("Unexpected C-state transition completion");
    cores[core].status = CoreStatus::Ready;
    cores[core].c_state = C1;
    cores[core].c_state_entered_at = Now();
    ++wake_completions;
    // Only dispatch queue entries here; other running cores may not yet have
    // advanced through their BeforeScheduler calls for this timestamp.
    ScheduleReadyWork(Now());
}

// Inputs: final simulation timestamp. Output: completion and energy metrics
// on stdout. Postcondition: verifies all jobs and contexts are finished before
// reporting; unfinished or owned work raises an exception.
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
