// Energy-first scheduler for the supplied eight-core simulator.
#include "scheduler.hpp"

#include <array>
#include <deque>
#include <iomanip>
#include <unordered_map>

// Compile-time controls allow paired benchmarks without changing the workload.
#ifndef EEC_SMALL_CORES
#define EEC_SMALL_CORES 4
#endif
#ifndef EEC_BIG_CORES
#define EEC_BIG_CORES 0
#endif
#ifndef EEC_PSTATE
#define EEC_PSTATE P3
#endif
#ifndef EEC_IDLE_STATE
#define EEC_IDLE_STATE C6
#endif
#ifndef EEC_TAIL_DVFS
#define EEC_TAIL_DVFS 1
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
};

std::array<CoreRecord, 8> cores;
std::deque<ProcessId_t> ready;
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

void Initialize() {
    if (initialized)
        return;
    initialized = true;
    for (CPUId_t core = 0; core < cores.size(); ++core) {
        cores[core].pid = InvalidProcessId();
        cores[core].enabled = static_cast<int>(core) < EEC_BIG_CORES ||
                              (core >= 4 && core < 4 + EEC_SMALL_CORES);
        // Constructors establish C1. Sleeping before requesting work also
        // avoids charging warm-spare power during the first wake interval.
        SleepCore(core);
    }
}

PState_t StateForWork(CPUId_t core, Time_t remaining) {
    if (EEC_TAIL_DVFS && EEC_PSTATE == P3) {
        // At a timer boundary, P4 completes <=120 small-core work units
        // (<=200 on a big core) in one quantum for less energy than P3.
        const Time_t p4_work_per_quantum = core >= 4 ? QUANTUM * 12 / 100 : QUANTUM / 5;
        if (remaining > 0 && remaining <= p4_work_per_quantum)
            return P4;
    }
    return EEC_PSTATE;
}

void Dispatch(ProcessId_t pid, CPUId_t core) {
    CoreRecord &record = cores[core];
    if (record.status != CoreStatus::Ready || record.pid != InvalidProcessId())
        ThrowException("Dispatch requires an unowned core in C1");
    auto process = processes.find(pid);
    if (process == processes.end())
        ThrowException("Dispatch received an unknown process");

    if (!process->second.dispatched) {
        const Time_t wait = Now() - process->second.arrival;
        total_wait += wait;
        if (wait > maximum_wait)
            maximum_wait = wait;
        process->second.dispatched = true;
    }
    const PState_t state = StateForWork(core, GetRemaining(pid));
    LoadContext(pid, core);
    RunCore(core);
    // RunCore resets the execution timestamp. No work is double-counted
    // when SetPState is applied immediately afterward at the same time.
    SetPState(core, state);
    record.pid = pid;
    record.pstate = state;
    record.status = CoreStatus::Running;
}

void ScheduleReadyWork() {
    // Prefer ready small cores before experimental big-core configurations.
    for (unsigned position = 0; position < cores.size(); ++position) {
        const CPUId_t core = (position + 4) % cores.size();
        if (!ready.empty() && cores[core].enabled && cores[core].status == CoreStatus::Ready) {
            const ProcessId_t pid = ready.front();
            ready.pop_front();
            Dispatch(pid, core);
        }
    }

    std::size_t waking = 0;
    for (const CoreRecord &record : cores)
        if (record.status == CoreStatus::Waking)
            ++waking;

    for (unsigned position = 0; position < cores.size() && waking < ready.size(); ++position) {
        const CPUId_t core = (position + 4) % cores.size();
        if (cores[core].enabled && cores[core].status == CoreStatus::Sleeping) {
            // A waking core is counted as future capacity. Never reissue this
            // request: the simulator would restart its transition countdown.
            cores[core].status = CoreStatus::Waking;
            ++wake_requests;
            SetCState(core, C1);
            ++waking;
        }
    }

    for (CPUId_t core = 0; core < cores.size(); ++core)
        SleepCore(core);
}
} // namespace

void CreateProcess(ProcessId_t pid) {
    Initialize();
    const Time_t work = GetRemaining(pid);
    if (work < 0 || !processes.emplace(pid, ProcessRecord{Now()}).second)
        ThrowException("Invalid or duplicate process creation");
    ++created;
    initial_work += static_cast<std::uint64_t>(work);
    ready.push_back(pid);
    ScheduleReadyWork();
}

void ExitProcess(ProcessId_t pid) {
    CPUId_t owner = static_cast<CPUId_t>(cores.size());
    for (CPUId_t core = 0; core < cores.size(); ++core)
        if (cores[core].status == CoreStatus::Running && cores[core].pid == pid)
            owner = core;
    if (owner == cores.size() || GetRemaining(pid) != 0 || processes.erase(pid) != 1)
        ThrowException("Completion does not match a running process");

    // BeforeScheduler has already detached this completed PID and set C1.
    // SaveContext here would incorrectly require a still-running C0 core.
    cores[owner].pid = InvalidProcessId();
    cores[owner].status = CoreStatus::Ready;
    ++completed;
    ScheduleReadyWork();
}

void TimerInterrupt(Time_t /* now */) {
    Initialize();
    if (EEC_TAIL_DVFS && EEC_PSTATE == P3) {
        for (CPUId_t core = 0; core < cores.size(); ++core) {
            CoreRecord &record = cores[core];
            if (record.status != CoreStatus::Running)
                continue;
            const PState_t state = StateForWork(core, GetRemaining(record.pid));
            if (state != record.pstate) {
                // BeforeScheduler already accounted for this quantum, but
                // AfterScheduler has not yet reset the execution timestamp.
                // A direct SetPState would subtract that work a second time.
                SaveContext(record.pid, core);
                LoadContext(record.pid, core);
                RunCore(core);
                SetPState(core, state);
                record.pstate = state;
                ++tail_changes;
            }
        }
    }
    ScheduleReadyWork();
}

void CStateTransitionComplete(CPUId_t core) {
    if (core >= cores.size() || cores[core].status != CoreStatus::Waking)
        ThrowException("Unexpected C-state transition completion");
    cores[core].status = CoreStatus::Ready;
    ++wake_completions;
    // Only dispatch queue entries here; other running cores may not yet have
    // advanced through their BeforeScheduler calls for this timestamp.
    ScheduleReadyWork();
}

void SimulationComplete(Time_t now) {
    if (!ready.empty() || !processes.empty() || created != completed)
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
