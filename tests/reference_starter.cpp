// Original single-big-core round-robin policy, kept as a benchmark reference.
#include <queue>
#include "scheduler.hpp"

namespace {
std::queue<ProcessId_t> ready;
ProcessId_t running = InvalidProcessId();
}

void CreateProcess(ProcessId_t pid) {
    if (running == InvalidProcessId()) {
        running = pid;
        LoadContext(running, 0);
        RunCore(0);
    } else {
        ready.push(pid);
    }
}

void ExitProcess(ProcessId_t pid) {
    if (running != pid)
        ThrowException("A process that was not running is calling exit");
    if (!ready.empty()) {
        running = ready.front();
        ready.pop();
        LoadContext(running, 0);
        RunCore(0);
    } else {
        running = InvalidProcessId();
    }
}

void TimerInterrupt(Time_t) {
    if (running == InvalidProcessId() || ready.empty())
        return;
    SaveContext(running, 0);
    ready.push(running);
    running = ready.front();
    ready.pop();
    LoadContext(running, 0);
    RunCore(0);
}

void CStateTransitionComplete(CPUId_t) {}

void SimulationComplete(Time_t now) {
    std::cout << "Run stopped at " << FormatTime(now) << " after consuming "
              << GetTotalEnergyConsumed() / 3600000000.0 << " kWh\n";
}
