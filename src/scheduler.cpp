//
// scheduler.cpp
// Energy-aware heterogeneous scheduler
//
// Developed with assistance from OpenAI ChatGPT. The scheduling strategy
// and parameter choices were evaluated using the provided project simulator.
//
// Policy:
//   - Classify jobs by remaining CPU time (short <= 12,000).
//   - Prefer big cores for short jobs and small cores for long jobs.
//   - Run small cores at P4 and big cores at P3.
//   - Put unused cores in C6 (race to idle).
//   - On each timer interrupt, preempt only when a waiting job has at least
//     5,000 less remaining work than the currently running job.
//

#include <algorithm>
#include <vector>
#include "scheduler.hpp"

namespace {

constexpr int NUM_CORES = 8;
constexpr Time_t SIZE_THRESHOLD = 12000;
constexpr Time_t PREEMPT_GAP = 5000;

// Scheduling preference order: small cores first, then big cores.
constexpr CPUId_t CORE_ORDER[NUM_CORES] = {4, 5, 6, 7, 0, 1, 2, 3};

std::vector<ProcessId_t> shortQueue;
std::vector<ProcessId_t> longQueue;

ProcessId_t running[NUM_CORES];
bool sleeping[NUM_CORES];
bool waking[NUM_CORES];
bool initialized = false;

bool IsSmallCore(int index) {
    return CORE_ORDER[index] >= 4;
}

bool IsShortJob(ProcessId_t pid) {
    return GetRemaining(pid) <= SIZE_THRESHOLD;
}

void InitializeScheduler() {
    shortQueue.clear();
    longQueue.clear();

    for (int i = 0; i < NUM_CORES; ++i) {
        running[i] = InvalidProcessId();
        sleeping[i] = (i != 0);
        waking[i] = false;
    }

    // Keep small core 4 ready initially; power off all other cores.
    for (int i = 1; i < NUM_CORES; ++i) {
        SetCState(CORE_ORDER[i], C6);
    }

    initialized = true;
}

void Enqueue(ProcessId_t pid) {
    if (IsShortJob(pid)) {
        shortQueue.push_back(pid);
    } else {
        longQueue.push_back(pid);
    }
}

size_t ReadyCount() {
    return shortQueue.size() + longQueue.size();
}

size_t ShortestPosition(const std::vector<ProcessId_t>& queue) {
    size_t best = 0;
    Time_t bestRemaining = GetRemaining(queue[0]);

    for (size_t i = 1; i < queue.size(); ++i) {
        Time_t remaining = GetRemaining(queue[i]);
        if (remaining < bestRemaining) {
            bestRemaining = remaining;
            best = i;
        }
    }

    return best;
}

ProcessId_t RemoveAt(std::vector<ProcessId_t>& queue, size_t position) {
    ProcessId_t pid = queue[position];
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(position));
    return pid;
}

// Big cores prefer short jobs; small cores prefer long jobs.
// If that class is empty, use the other class so an awake core does not
// remain idle while work is waiting.
ProcessId_t PopForCore(int index) {
    if (IsSmallCore(index)) {
        if (!longQueue.empty()) {
            return RemoveAt(longQueue, ShortestPosition(longQueue));
        }
        return RemoveAt(shortQueue, ShortestPosition(shortQueue));
    }

    if (!shortQueue.empty()) {
        return RemoveAt(shortQueue, ShortestPosition(shortQueue));
    }
    return RemoveAt(longQueue, ShortestPosition(longQueue));
}

ProcessId_t PeekForCore(int index) {
    if (IsSmallCore(index)) {
        if (!longQueue.empty()) {
            return longQueue[ShortestPosition(longQueue)];
        }
        return shortQueue[ShortestPosition(shortQueue)];
    }

    if (!shortQueue.empty()) {
        return shortQueue[ShortestPosition(shortQueue)];
    }
    return longQueue[ShortestPosition(longQueue)];
}

bool RemoveReadyProcess(ProcessId_t pid) {
    for (std::vector<ProcessId_t>* queue : {&shortQueue, &longQueue}) {
        auto it = std::find(queue->begin(), queue->end(), pid);
        if (it != queue->end()) {
            queue->erase(it);
            return true;
        }
    }
    return false;
}

void StartProcess(ProcessId_t pid, int index) {
    running[index] = pid;

    LoadContext(pid, CORE_ORDER[index]);
    RunCore(CORE_ORDER[index]);
    SetPState(CORE_ORDER[index], IsSmallCore(index) ? P4 : P3);
}

int FindSleepingCore(bool small) {
    for (int i = 0; i < NUM_CORES; ++i) {
        if (IsSmallCore(i) == small && sleeping[i] && !waking[i]) {
            return i;
        }
    }
    return -1;
}

int FindAnySleepingCore() {
    for (int i = 0; i < NUM_CORES; ++i) {
        if (sleeping[i] && !waking[i]) {
            return i;
        }
    }
    return -1;
}

int ChooseCoreToWake() {
    if (ReadyCount() == 0) {
        return -1;
    }

    // Match waiting job type to its preferred core type when possible.
    if (!shortQueue.empty()) {
        int bigCore = FindSleepingCore(false);
        if (bigCore >= 0) {
            return bigCore;
        }
    }

    if (!longQueue.empty()) {
        int smallCore = FindSleepingCore(true);
        if (smallCore >= 0) {
            return smallCore;
        }
    }

    return FindAnySleepingCore();
}

void WakeOneIfUseful() {
    int index = ChooseCoreToWake();
    if (index < 0) {
        return;
    }

    waking[index] = true;
    SetCState(CORE_ORDER[index], C1);
}

int FindRunningCore(ProcessId_t pid) {
    for (int i = 0; i < NUM_CORES; ++i) {
        if (running[i] == pid) {
            return i;
        }
    }
    return -1;
}

int FindCoreIndex(CPUId_t coreId) {
    for (int i = 0; i < NUM_CORES; ++i) {
        if (CORE_ORDER[i] == coreId) {
            return i;
        }
    }
    return -1;
}

}  // namespace

void CreateProcess(ProcessId_t pid) {
    if (!initialized) {
        InitializeScheduler();
    }

    // Reuse an already-awake idle core before waking another one.
    for (int i = 0; i < NUM_CORES; ++i) {
        if (running[i] == InvalidProcessId() && !sleeping[i] && !waking[i]) {
            StartProcess(pid, i);
            return;
        }
    }

    Enqueue(pid);
    WakeOneIfUseful();
}

void ExitProcess(ProcessId_t pid) {
    int index = FindRunningCore(pid);
    if (index < 0) {
        ThrowException("Exit from process that is not running");
        return;
    }

    if (ReadyCount() > 0) {
        StartProcess(PopForCore(index), index);
    } else {
        // No ready work: shut this core down until it is needed again.
        running[index] = InvalidProcessId();
        sleeping[index] = true;
        waking[index] = false;
        SetCState(CORE_ORDER[index], C6);
    }

    WakeOneIfUseful();
}

void TimerInterrupt(Time_t now) {
    (void)now;

    if (!initialized || ReadyCount() == 0) {
        return;
    }

    for (int index = 0; index < NUM_CORES && ReadyCount() > 0; ++index) {
        if (running[index] == InvalidProcessId()) {
            continue;
        }

        ProcessId_t candidate = PeekForCore(index);
        Time_t candidateRemaining = GetRemaining(candidate);
        Time_t runningRemaining = GetRemaining(running[index]);

        // Switch only when the waiting job is substantially shorter.
        if (candidateRemaining + PREEMPT_GAP >= runningRemaining) {
            continue;
        }

        ProcessId_t old = running[index];
        SaveContext(old, CORE_ORDER[index]);

        if (!RemoveReadyProcess(candidate)) {
            ThrowException("Preemption candidate disappeared from ready queues");
            return;
        }

        Enqueue(old);

        // LoadContext requires C1.
        SetCState(CORE_ORDER[index], C1);
        StartProcess(candidate, index);
    }
}

void CStateTransitionComplete(CPUId_t core_id) {
    if (!initialized) {
        return;
    }

    int index = FindCoreIndex(core_id);
    if (index < 0 || !waking[index]) {
        return;
    }

    sleeping[index] = false;
    waking[index] = false;

    if (ReadyCount() > 0 && running[index] == InvalidProcessId()) {
        StartProcess(PopForCore(index), index);
    }

    // If work remains queued, begin waking another appropriate core.
    WakeOneIfUseful();
}

void SimulationComplete(Time_t now) {
    const double energy = GetTotalEnergyConsumed();

    const double energyKWh = energy / 3600000000.0;
    const double timeHours = static_cast<double>(now) / 3600000.0;
    const double edp = energyKWh * timeHours;

    std::cout << "Run stopped at "
              << FormatTime(now)
              << " after consuming "
              << energyKWh
              << " kWh"
              << std::endl;

    std::cout << "Energy-delay product: "
              << edp
              << std::endl;
}
