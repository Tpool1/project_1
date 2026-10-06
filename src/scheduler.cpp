// Energy-first scheduler for the supplied eight-core simulator.
#include "scheduler.hpp"
#include "ready_queue.hpp"

#include <array>
#include <iomanip>
#include <optional>
#include <unordered_map>

/*
 * Ideas:
 * 1. Intelligently choose between big and small cores. Big cores for more work unit jobs, small cores otherwise
 * 3. Adaptive idle control instead of always going to C6 for sleep. Look into methods to predict job arrivals
 * 4. Adjust Pstate based on IO bound operations
 */

// Compile-time controls allow paired benchmarks without changing the workload.
#ifndef EEC_SMALL_CORES
#define EEC_SMALL_CORES 4 // Number of small cores (IDs 4-7) enabled for work.
#endif
#ifndef EEC_BIG_CORES
#define EEC_BIG_CORES 3 // Number of big cores (IDs 0-3) enabled for work.
#endif
#ifndef EEC_PSTATE
#define EEC_PSTATE P3 // P-state used for normal execution.
#endif
#ifndef EEC_IDLE_STATE
#define EEC_IDLE_STATE C4 // Idle C-state for enabled cores; disabled cores use C6.
#endif
#ifndef EEC_TAIL_DVFS
#define EEC_TAIL_DVFS 1 // Enable P3-to-P4 switching when a job can finish in one quantum.
#endif

static_assert(EEC_SMALL_CORES >= 0 && EEC_SMALL_CORES <= 4, "Invalid small-core count");
static_assert(EEC_BIG_CORES >= 0 && EEC_BIG_CORES <= 4, "Invalid big-core count");
static_assert(EEC_SMALL_CORES + EEC_BIG_CORES > 0, "At least one core is needed");
static_assert(EEC_PSTATE >= P0 && EEC_PSTATE <= P4, "Invalid P-state");
static_assert(EEC_IDLE_STATE == C1 || EEC_IDLE_STATE == C4 || EEC_IDLE_STATE == C6,
              "Supported idle states are C1, C4, and C6");

namespace {
enum class CoreStatus { Ready, Running, Sleeping, Waking };

struct CoreRecord {
    CoreStatus status = CoreStatus::Ready;
    ProcessId_t pid = 0;
    PState_t pstate = P0;
    bool enabled = false;
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

// Inputs: core ID. Output: none. Postcondition: a ready core enters its
// configured idle state (enabled) or C6 (disabled); other cores are unchanged.
void SleepCore(CPUId_t core) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Ready)
        return;
    const CState_t state = record.enabled ? EEC_IDLE_STATE : C6;
    if (state != C1) {
        SetCState(core, state);
        record.status = CoreStatus::Sleeping;
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
    if (EEC_TAIL_DVFS) {
        // At a timer interrupt, P4 completes <=120 small-core work units
        // (<=200 on a big core) in one quantum for less energy than P3.
        // These thresholds come from libsim.so: CPU::BeforeScheduler applies
        // speed[P4] * scale[core type] * elapsed, with P4 speed 0.20, big
        // scale 1.0, and small scale 0.60. For QUANTUM=1000 (sim_types.h),
        // that is 200 work units on a big core and 120 on a small core.
        const Time_t p4_work_per_quantum = core >= 4 ? 120 : 200;
        if (remaining > 0 && remaining <= p4_work_per_quantum)
            return P4;
    }
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
        if (!ready.Empty() && cores[core].enabled && cores[core].status == CoreStatus::Ready) {
            const ProcessId_t pid = ready.PopNext();
            Dispatch(pid, core, now);
        }
    }

    // Are there any cores in the process of waking up?
    std::size_t waking = 0;
    for (const CoreRecord &record : cores)
        if (record.status == CoreStatus::Waking)
            ++waking;

    // while we are not out of cores to check and there are still more threads than ready cores
    for (unsigned position = 0; position < cores.size() && waking < ready.Size(); ++position) {
        const CPUId_t core = (position + 4) % cores.size();
        if (cores[core].enabled && cores[core].status == CoreStatus::Sleeping) {
            // A waking core is counted as future capacity. Never reissue this
            // request: the simulator would restart its transition countdown.
            cores[core].status = CoreStatus::Waking;
            ++wake_requests;
            SetCState(core, C1);
            ++waking; // count moving from C6 (idle) to C1 as waking up
        }
    }

    // Put unused (ready) cores to sleep
    for (CPUId_t core = 0; core < cores.size(); ++core)
        SleepCore(core);
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
    ++completed;
    ScheduleReadyWork(Now());
}

// Inputs: timer timestamp.
// Output: none. Postcondition: eligible running jobs use the appropriate
// final-interval P-state, then ready work is scheduled.
void TimerInterrupt(Time_t now) {
    Initialize();
    if (EEC_TAIL_DVFS) {
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
    }
    ScheduleReadyWork(now);
}

// Inputs: core ID whose wake transition completed. Output: none. Postcondition:
// the validated waking core becomes ready and queued work is rescheduled.
void CStateTransitionComplete(CPUId_t core) {
    if (core >= cores.size() || cores[core].status != CoreStatus::Waking)
        ThrowException("Unexpected C-state transition completion");
    cores[core].status = CoreStatus::Ready;
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
    const double lower_bound = static_cast<double>(initial_work) * (35.0 / 3.0);
    std::cout << std::setprecision(12)
              << "Run stopped at " << FormatTime(now) << " after consuming "
              << energy / 3600000000.0 << " kWh\n"
              << "Raw time: " << now << "; raw energy: " << energy
              << "; completed: " << completed << '/' << created << '\n'
              << "Mean/max dispatch wait (raw time): "
              << (created ? static_cast<double>(total_wait) / created : 0.0)
              << '/' << maximum_wait << "; wakes: " << wake_completions << '/' << wake_requests
              << "; tail P-state changes: " << tail_changes << '\n'
              << "Active-energy lower bound: " << lower_bound
              << "; overhead: " << (lower_bound ? 100.0 * (energy / lower_bound - 1.0) : 0.0)
              << "%\n";
}
