// Link only the callback adapter and this stub, not the real policy or CPU.
#include "interfaces.h"
#include "scheduling_algorithm.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
class RecordingAlgorithm final : public scheduling::Algorithm {
public:
    void OnProcessCreated(ProcessId_t pid) override { calls.emplace_back(0, pid); }
    void OnProcessExited(ProcessId_t pid) override { calls.emplace_back(1, pid); }
    void OnTimer(Time_t now) override { calls.emplace_back(2, now); }
    void OnCoreReady(CPUId_t core) override { calls.emplace_back(3, core); }
    scheduling::Statistics Finish() const override {
        calls.emplace_back(4, 0);
        finished = true;
        return {2, 2, 50.0, 100, 3, 3, 1, 1800000000.0};
    }

    mutable std::vector<std::pair<int, Time_t>> calls;
    mutable bool finished = false;
};
RecordingAlgorithm algorithm;
} // namespace

namespace scheduling {
Algorithm &ActiveAlgorithm() { return algorithm; }
} // namespace scheduling

double GetTotalEnergyConsumed() {
    if (!algorithm.finished)
        throw std::runtime_error("Energy reported before policy completion validation");
    return 3600000000.0;
}

std::string FormatTime(Time_t now) {
    if (now != 456)
        throw std::runtime_error("Completion timestamp was not preserved");
    return "formatted-time";
}

int main() {
    std::ostringstream output;
    std::streambuf *original = std::cout.rdbuf(output.rdbuf());
    try {
        CreateProcess(42);
        ExitProcess(42);
        TimerInterrupt(123);
        CStateTransitionComplete(4);
        SimulationComplete(456);

        const std::vector<std::pair<int, Time_t>> expected_calls = {
            {0, 42}, {1, 42}, {2, 123}, {3, 4}, {4, 0}
        };
        if (algorithm.calls != expected_calls)
            throw std::runtime_error("Adapter did not forward policy events exactly once");
        const std::string expected_output =
            "Run stopped at formatted-time after consuming 1 kWh\n"
            "Raw time: 456; raw energy: 3600000000; completed: 2/2\n"
            "EDP (raw energy × raw time): 1641600000000\n"
            "Mean/max dispatch wait (raw time): 50/100; wakes: 3/3; tail P-state changes: 1\n"
            "Active-energy lower bound: 1800000000; overhead: 100%\n";
        if (output.str() != expected_output)
            throw std::runtime_error("Adapter did not report the policy statistics correctly");
        std::cout.rdbuf(original);
        std::cout << "Scheduler adapter test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cout.rdbuf(original);
        std::cerr << "ADAPTER TEST FAILED: " << error.what() << '\n';
        return 1;
    }
}
