// Exercise the policy's idle lifecycle with one enabled core and a small
// simulator stub. Build with EEC_BIG_CORES=0 and EEC_SMALL_CORES=1.
#include "interfaces.h"

#include <array>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
Time_t clock_now = 0;
std::array<CState_t, 8> states{};
std::array<bool, 8> pending{};
std::array<ProcessId_t, 8> owners{};
std::map<ProcessId_t, Time_t> work;

struct Change { Time_t time; CPUId_t core; CState_t state; };
std::vector<Change> changes;

void Check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void Tick(Time_t time) {
    clock_now = time;
    TimerInterrupt(time);
}

void FinishTransition(Time_t time) {
    clock_now = time;
    Check(pending[4], "Expected an asynchronous transition");
    pending[4] = false;
    CStateTransitionComplete(4);
}

void ExpectChange(std::size_t count, Time_t time, CState_t state) {
    Check(changes.size() == count, "Unexpected number of state requests");
    Check(changes.back().time == time && changes.back().core == 4 &&
          changes.back().state == state, "Wrong state or timeout boundary");
}

void FinishJob(ProcessId_t pid, Time_t time) {
    clock_now = time;
    Check(owners[4] == pid, "Job was not dispatched to the enabled core");
    work[pid] = 0;
    owners[4] = InvalidProcessId();
    states[4] = C1; // The simulator does this before ExitProcess.
    ExitProcess(pid);
}
} // namespace

Time_t Now() { return clock_now; }
ProcessId_t InvalidProcessId() { return 0; }
Time_t GetRemaining(ProcessId_t pid) { return work.at(pid); }
void ThrowException(std::string message) { throw std::runtime_error(message); }
std::string FormatTime(Time_t) { return ""; }
double GetTotalEnergyConsumed() { return 0; }

void SetCState(CPUId_t core, CState_t state) {
    Check(!pending[core], "State request restarted an unfinished transition");
    Check(owners[core] == InvalidProcessId(), "Sleeping an occupied core");
    const CState_t previous = states[core];
    changes.push_back({clock_now, core, state});
    pending[core] = previous == C3 || previous == C4 || previous == C6;
    states[core] = state;
}
void LoadContext(ProcessId_t pid, CPUId_t core) {
    Check(states[core] == C1 && owners[core] == InvalidProcessId(),
          "Dispatch requires an unoccupied C1 core");
    owners[core] = pid;
}
void RunCore(CPUId_t core) { states[core] = C0; }
void SetPState(CPUId_t, PState_t) {}
void SaveContext(ProcessId_t, CPUId_t) { throw std::runtime_error("Unexpected preemption"); }

int main() {
    try {
        states.fill(C1);
        owners.fill(InvalidProcessId());
        Tick(0);
        Check(states[4] == C1, "Core should begin idle in C1");
        Tick(2999);
        Check(states[4] == C1, "C1 ended before three quanta");
        Tick(3000);
        ExpectChange(8, 3000, C2); // Seven disabled cores enter C6 at time zero.
        Tick(7999);
        Check(states[4] == C2, "C2 ended before five more quanta");
        Tick(8000);
        ExpectChange(9, 8000, C3);
        Tick(12999);
        Check(states[4] == C3, "C3 ended before five more quanta");
        Tick(13000);
        ExpectChange(10, 13000, C4);

        clock_now = 14000;
        work[1] = 100;
        CreateProcess(1);
        Check(changes.size() == 10, "Demand interrupted C4 entry");
        FinishTransition(23000);
        ExpectChange(11, 23000, C1);
        Tick(24000);
        Check(changes.size() == 11, "Wake request was repeated");
        FinishTransition(33000);
        Check(owners[4] == 1, "Queued work was not dispatched after wake");
        FinishJob(1, 34000);

        Tick(36999);
        Check(states[4] == C1, "Idle clock did not reset after completion");
        Tick(37000);
        ExpectChange(12, 37000, C2);
        clock_now = 38000;
        work[2] = 100;
        CreateProcess(2);
        ExpectChange(13, 38000, C1);
        Check(owners[4] == 2, "C2 should wake and dispatch synchronously");
        FinishJob(2, 39000);

        Tick(42000);
        ExpectChange(14, 42000, C2);
        Tick(47000);
        ExpectChange(15, 47000, C3);
        Tick(52000);
        ExpectChange(16, 52000, C4);
        FinishTransition(62000);
        Check(changes.size() == 16, "Idle C4 completion requested a wake");
        Tick(71999);
        Check(states[4] == C4, "C4 ended before ten more quanta");
        Tick(72000);
        ExpectChange(17, 72000, C6);
        Tick(73000);
        Check(changes.size() == 17, "C6 entry request was repeated");
        FinishTransition(82000);
        clock_now = 83000;
        work[3] = 100;
        CreateProcess(3);
        ExpectChange(18, 83000, C1);
        Tick(84000);
        Check(changes.size() == 18, "C6 wake request was repeated");
        FinishTransition(2083000);
        Check(owners[4] == 3, "Queued work was not dispatched after C6 wake");
        std::cout << "Idle timeout lifecycle test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "IDLE TIMEOUT TEST FAILED: " << error.what() << '\n';
        return 1;
    }
}
