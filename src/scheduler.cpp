//
// scheduler.cpp
// Energy-aware heterogeneous scheduler
// Developed with assistance from OpenAI ChatGPT; the algorithm and
// parameters were evaluated using the provided project simulator.
//
// Strategy:
//   - Prefer small cores for long jobs and big cores for short jobs.
//   - Small cores run at P4; big cores run at P3.
//   - Unused cores enter C6 (race to idle).
//   - A job is "short" when its remaining CPU time is <= 12000.
//   - On every timer interrupt, preempt only when the best waiting job
//     has at least 5000 less remaining work than the running job.
//

#include <algorithm>
#include <vector>
#include "scheduler.hpp"

struct Job {
    ProcessId_t pid;
};

// Candidate-core order: small cores first, then big cores.
static const CPUId_t CORES[8] = {4, 5, 6, 7, 0, 1, 2, 3};

static const Time_t SIZE_THRESHOLD = 12000;
static const Time_t PREEMPT_GAP = 5000;

static std::vector<Job> shortQ;
static std::vector<Job> longQ;

static ProcessId_t running[8];
static bool sleeping[8];
static bool waking[8];
static bool initialized = false;

static bool IsSmallCore(int index) {
    return CORES[index] >= 4;
}

static bool IsShort(ProcessId_t pid) {
    return GetRemaining(pid) <= SIZE_THRESHOLD;
}

static void InitializeScheduler() {
    shortQ.clear();
    longQ.clear();

    for (int i = 0; i < 8; ++i) {
        running[i] = InvalidProcessId();
        sleeping[i] = (i != 0);
        waking[i] = false;
    }

    // Keep one efficient small core (core 4) immediately available.
    // Every other core starts powered off and is woken only when useful.
    for (int i = 1; i < 8; ++i) {
        SetCState(CORES[i], C6);
    }

    initialized = true;
}

static void Enqueue(ProcessId_t pid) {
    if (IsShort(pid)) {
        shortQ.push_back({pid});
    } else {
        longQ.push_back({pid});
    }
}

static size_t ReadyCount() {
    return shortQ.size() + longQ.size();
}

static size_t ShortestPosition(const std::vector<Job>& q) {
    size_t best = 0;
    Time_t bestRemaining = GetRemaining(q[0].pid);

    for (size_t i = 1; i < q.size(); ++i) {
        Time_t remaining = GetRemaining(q[i].pid);
        if (remaining < bestRemaining) {
            bestRemaining = remaining;
            best = i;
        }
    }

    return best;
}

static Job RemoveAt(std::vector<Job>& q, size_t position) {
    Job job = q[position];
    q.erase(q.begin() + static_cast<std::ptrdiff_t>(position));
    return job;
}

// Short jobs prefer big cores; long jobs prefer small cores.
// If the preferred class is empty, use the other class so an awake core
// never sits idle while useful work is waiting.
static Job PopForCore(int index) {
    if (IsSmallCore(index)) {
        if (!longQ.empty()) {
            return RemoveAt(longQ, ShortestPosition(longQ));
        }
        return RemoveAt(shortQ, ShortestPosition(shortQ));
    }

    if (!shortQ.empty()) {
        return RemoveAt(shortQ, ShortestPosition(shortQ));
    }
    return RemoveAt(longQ, ShortestPosition(longQ));
}

static ProcessId_t PeekForCore(int index) {
    if (IsSmallCore(index)) {
        if (!longQ.empty()) {
            return longQ[ShortestPosition(longQ)].pid;
        }
        return shortQ[ShortestPosition(shortQ)].pid;
    }

    if (!shortQ.empty()) {
        return shortQ[ShortestPosition(shortQ)].pid;
    }
    return longQ[ShortestPosition(longQ)].pid;
}

static bool RemoveReadyProcess(ProcessId_t pid) {
    for (std::vector<Job>* q : {&shortQ, &longQ}) {
        auto it = std::find_if(q->begin(), q->end(),
                               [pid](const Job& job) { return job.pid == pid; });
        if (it != q->end()) {
            q->erase(it);
            return true;
        }
    }
    return false;
}

static void StartProcess(ProcessId_t pid, int index) {
    running[index] = pid;

    LoadContext(pid, CORES[index]);
    RunCore(CORES[index]);

    // Measured robust operating points from the project experiments.
    SetPState(CORES[index], IsSmallCore(index) ? P4 : P3);
}

static int FindSleepingCore(bool small) {
    for (int i = 0; i < 8; ++i) {
        if (IsSmallCore(i) == small && sleeping[i] && !waking[i]) {
            return i;
        }
    }
    return -1;
}

static int FindAnySleepingCore() {
    for (int i = 0; i < 8; ++i) {
        if (sleeping[i] && !waking[i]) {
            return i;
        }
    }
    return -1;
}

static int ChooseCoreToWake() {
    if (ReadyCount() == 0) {
        return -1;
    }

    // Prefer the core type that matches the waiting job class.
    if (!shortQ.empty()) {
        int big = FindSleepingCore(false);
        if (big >= 0) {
            return big;
        }
    }

    if (!longQ.empty()) {
        int small = FindSleepingCore(true);
        if (small >= 0) {
            return small;
        }
    }

    // If the preferred type is unavailable, wake any remaining core rather
    // than leave work queued unnecessarily.
    return FindAnySleepingCore();
}

static void WakeOneIfUseful() {
    int index = ChooseCoreToWake();
    if (index < 0) {
        return;
    }

    waking[index] = true;
    SetCState(CORES[index], C1);
}

void CreateProcess(ProcessId_t pid) {
    if (!initialized) {
        InitializeScheduler();
    }

    // Reuse an already-awake idle core before paying a wake-up penalty.
    for (int i = 0; i < 8; ++i) {
        if (running[i] == InvalidProcessId() && !sleeping[i] && !waking[i]) {
            StartProcess(pid, i);
            return;
        }
    }

    Enqueue(pid);
    WakeOneIfUseful();
}

void ExitProcess(ProcessId_t pid) {
    int index = -1;

    for (int i = 0; i < 8; ++i) {
        if (running[i] == pid) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        ThrowException("Exit from process that is not running");
        return;
    }

    if (ReadyCount() > 0) {
        Job next = PopForCore(index);
        StartProcess(next.pid, index);
    } else {
        // Race to idle when there is no immediately useful work.
        running[index] = InvalidProcessId();
        sleeping[index] = true;
        waking[index] = false;
        SetCState(CORES[index], C6);
    }

    WakeOneIfUseful();
}

void TimerInterrupt(Time_t now) {
    (void)now;

    if (!initialized || ReadyCount() == 0) {
        return;
    }

    // Winning configuration: evaluate selective preemption every timer tick.
    for (int index = 0; index < 8 && ReadyCount() > 0; ++index) {
        if (running[index] == InvalidProcessId()) {
            continue;
        }

        ProcessId_t candidate = PeekForCore(index);
        Time_t candidateRemaining = GetRemaining(candidate);
        Time_t runningRemaining = GetRemaining(running[index]);

        // Only pay the context-switch cost for a substantially shorter job.
        if (candidateRemaining + PREEMPT_GAP >= runningRemaining) {
            continue;
        }

        ProcessId_t old = running[index];
        SaveContext(old, CORES[index]);

        if (!RemoveReadyProcess(candidate)) {
            ThrowException("Preemption candidate disappeared from ready queues");
            return;
        }

        Enqueue(old);

        // LoadContext requires C1.
        SetCState(CORES[index], C1);
        StartProcess(candidate, index);
    }
}

void CStateTransitionComplete(CPUId_t core_id) {
    if (!initialized) {
        return;
    }

    int index = -1;
    for (int i = 0; i < 8; ++i) {
        if (CORES[i] == core_id) {
            index = i;
            break;
        }
    }

    if (index < 0 || !waking[index]) {
        return;
    }

    sleeping[index] = false;
    waking[index] = false;

    if (ReadyCount() > 0 && running[index] == InvalidProcessId()) {
        Job next = PopForCore(index);
        StartProcess(next.pid, index);
    }

    // If more work remains, begin waking another useful core.
    WakeOneIfUseful();
}

void SimulationComplete(Time_t now) {
    std::cout << "Run stopped at "
              << FormatTime(now)
              << " after consuming "
              << GetTotalEnergyConsumed() / 3600000000.0
              << " kWh"
              << std::endl;
}
