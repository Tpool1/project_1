// Link with renamed scheduler callbacks so every policy sees identical input.
#include "interfaces.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <stdexcept>

void PolicyCreateProcess(ProcessId_t);
void PolicyExitProcess(ProcessId_t);
void PolicyTimerInterrupt(Time_t);
void PolicyCStateTransitionComplete(CPUId_t);
void PolicySimulationComplete(Time_t);

// GNU ld wraps only the scheduler's calls; the hardware still performs its
// own completion bookkeeping. This makes ownership assertions independent.
extern "C" void __real__Z11LoadContextjj(ProcessId_t, CPUId_t);
extern "C" void __real__Z11SaveContextjj(ProcessId_t, CPUId_t);

namespace {
struct Job { Time_t arrival; Time_t work; };
std::map<ProcessId_t, Job> live;
std::array<ProcessId_t, 8> owned;
std::uint64_t created = 0, completed = 0, work = 0;
std::uint64_t quantized_floor = 0;
Time_t maximum_turnaround = 0;
Time_t first_callback = -1;
long double turnaround = 0;
std::string scenario;
bool expect_optimum = false;

void ObserveCallback() {
    if (first_callback < 0)
        first_callback = Now();
}

#ifdef EEC_CHECK_CALLBACK_WORK
std::map<ProcessId_t, Time_t> Snapshot() {
    std::map<ProcessId_t, Time_t> result;
    for (ProcessId_t pid : owned)
        if (pid != InvalidProcessId())
            result.emplace(pid, GetRemaining(pid));
    return result;
}
void CheckUnchanged(const std::map<ProcessId_t, Time_t> &snapshot) {
    for (const auto &entry : snapshot)
        if (GetRemaining(entry.first) != entry.second)
            throw std::runtime_error("Scheduler callback changed remaining work");
}
#endif
} // namespace

extern "C" void __wrap__Z11LoadContextjj(ProcessId_t pid, CPUId_t core) {
    if (core >= owned.size() || owned[core] != InvalidProcessId() || live.count(pid) != 1)
        throw std::runtime_error("Invalid context load or occupied destination");
    for (ProcessId_t owner : owned)
        if (owner == pid)
            throw std::runtime_error("Process loaded onto more than one core");
    __real__Z11LoadContextjj(pid, core);
    owned[core] = pid;
}

extern "C" void __wrap__Z11SaveContextjj(ProcessId_t pid, CPUId_t core) {
    if (core >= owned.size() || owned[core] != pid)
        throw std::runtime_error("Context save does not match CPU ownership");
    __real__Z11SaveContextjj(pid, core);
    owned[core] = InvalidProcessId();
}

void CreateProcess(ProcessId_t pid) {
    ObserveCallback();
    const Time_t remaining = GetRemaining(pid);
    if (remaining < 0 || !live.emplace(pid, Job{Now(), remaining}).second)
        throw std::runtime_error("Invalid process creation");
    ++created;
    work += remaining;
    // Minimum for one job using full timer intervals on small cores. P0-P2
    // are dominated in energy by combinations of P3/P4 at this granularity.
    const Time_t remainder = remaining % 240;
    quantized_floor += (remaining / 240) * 2800 +
                       (remainder == 0 ? 0 : remainder <= 120 ? 1800 : 2800);
#ifdef EEC_CHECK_CALLBACK_WORK
    const auto snapshot = Snapshot();
#endif
    PolicyCreateProcess(pid);
#ifdef EEC_CHECK_CALLBACK_WORK
    CheckUnchanged(snapshot);
#endif
}

void ExitProcess(ProcessId_t pid) {
    ObserveCallback();
    const auto job = live.find(pid);
    if (job == live.end() || GetRemaining(pid) != 0)
        throw std::runtime_error("Unknown or unfinished exit");
    unsigned owners = 0;
    for (ProcessId_t &owner : owned)
        if (owner == pid) {
            owner = InvalidProcessId();
            ++owners;
        }
    if (owners != 1)
        throw std::runtime_error("Exiting process does not have exactly one owner");
    const Time_t elapsed = Now() - job->second.arrival;
    turnaround += elapsed;
    if (elapsed > maximum_turnaround)
        maximum_turnaround = elapsed;
    live.erase(job);
    ++completed;
#ifdef EEC_CHECK_CALLBACK_WORK
    const auto snapshot = Snapshot();
#endif
    PolicyExitProcess(pid);
#ifdef EEC_CHECK_CALLBACK_WORK
    CheckUnchanged(snapshot);
#endif
}

void TimerInterrupt(Time_t now) {
    ObserveCallback();
#ifdef EEC_CHECK_CALLBACK_WORK
    const auto snapshot = Snapshot();
#endif
    PolicyTimerInterrupt(now);
#ifdef EEC_CHECK_CALLBACK_WORK
    CheckUnchanged(snapshot);
#endif
}

void CStateTransitionComplete(CPUId_t core) {
    ObserveCallback();
#ifdef EEC_CHECK_CALLBACK_WORK
    const auto snapshot = Snapshot();
#endif
    PolicyCStateTransitionComplete(core);
#ifdef EEC_CHECK_CALLBACK_WORK
    CheckUnchanged(snapshot);
#endif
}

void SimulationComplete(Time_t now) {
    if (!live.empty() || created != completed)
        throw std::runtime_error("Simulation left unfinished work");
    for (ProcessId_t pid : owned)
        if (pid != InvalidProcessId())
            throw std::runtime_error("Simulation left an occupied CPU context");
    PolicySimulationComplete(now);
    const double energy = GetTotalEnergyConsumed();
    const double floor = work * (35.0 / 3.0);
    // All eight cores start in C1 until the first scheduler upcall. The
    // supplied workloads have arrivals and executions on the timer grid.
    const double startup_floor = 57.6 * first_callback;
    if (!std::isfinite(energy) || energy + 1e-5 < floor)
        throw std::runtime_error("Energy fell below the work-accounting lower bound");
    if (expect_optimum && std::abs(energy - quantized_floor - startup_floor) > 0.01)
        throw std::runtime_error("Policy did not match the full-quantum energy minimum");
    std::cout << std::setprecision(17) << "RESULT {\"scenario\":\"" << scenario
              << "\",\"time\":" << now << ",\"energy\":" << energy
              << ",\"kwh\":" << energy / 3600000000.0
              << ",\"created\":" << created << ",\"completed\":" << completed
              << ",\"work\":" << work << ",\"floor\":" << floor
              << ",\"quantized_floor\":" << quantized_floor
              << ",\"startup_floor\":" << startup_floor
              << ",\"mean_turnaround\":" << (created ? turnaround / created : 0)
              << ",\"max_turnaround\":" << maximum_turnaround << "}\n";
}

int main(int argc, char **argv) {
    try {
        scenario = argc > 1 ? argv[1] : "seed:0";
        expect_optimum = argc > 2 && std::string(argv[2]) == "--expect-minimum";
        if (scenario.rfind("seed:", 0) == 0) {
            GenerateProcesses(static_cast<unsigned>(std::stoul(scenario.substr(5))));
        } else if (scenario == "empty") {
            // The initial timer still invokes the scheduler once.
        } else if (scenario == "single") {
            AddProcess(0, 1);
        } else if (scenario == "long") {
            AddProcess(0, 10000);
        } else if (scenario == "burst") {
            for (unsigned i = 0; i < 32; ++i)
                AddProcess(0, (i + 1) * 7);
        } else if (scenario == "gaps") {
            for (unsigned i = 0; i < 12; ++i)
                AddProcess(i * 5000, 3 + i % 3);
        } else if (scenario == "mixed") {
            AddProcess(0, 10000);
            for (unsigned i = 0; i < 100; ++i)
                AddProcess(i * 37, 1 + i % 8);
        } else if (scenario == "boundary") {
            for (unsigned i = 0; i < 80; ++i)
                AddProcess((i / 4) * 2000 + i % 4, 1 + i % 6);
        } else if (scenario == "late") {
            AddProcess(5012, 1);
        } else if (scenario == "zero") {
            AddProcess(0, 0);
            AddProcess(0, 1);
        } else {
            throw std::runtime_error("Unknown benchmark scenario");
        }
        owned.fill(InvalidProcessId());
        InitCores();
        ScheduleTimer(Now() + QUANTUM);
        Simulate();
    } catch (const std::exception &error) {
        std::cerr << "BENCHMARK FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
