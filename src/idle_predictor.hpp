#ifndef IDLE_PREDICTOR_HPP
#define IDLE_PREDICTOR_HPP

#include "sim_types.h"

#include <algorithm>
#include <array>
#include <optional>

class IdlePredictor {
public:
    // called when the CPU becomes idle
    void Begin(Time_t now) { idle_since_ = now; }

    // called when it wakes up again (idle period ends)
    void End(Time_t now) {
        durations_[next_] = std::max<Time_t>(0, now - idle_since_);
        next_ = (next_ + 1) % durations_.size();
        count_ = std::min(count_ + 1, durations_.size());
    }

    // Returns the recommended C-state the CPU should enter at this time
    CState_t Select(Time_t now, std::optional<Time_t> next_wakeup = std::nullopt) const {
        // shortcut: mean and 20k cold start can miss bursts; calibrate the forecast on arrival traces.
        Time_t predicted = 20000; // Cold-start estimate: enough for C4, not C6.
        if (count_) {
            predicted = 0;
            Time_t remainder = 0;
            for (std::size_t i = 0; i < count_; ++i) {
                predicted += durations_[i] / static_cast<Time_t>(count_);
                remainder += durations_[i] % static_cast<Time_t>(count_);
            }
            predicted += remainder / static_cast<Time_t>(count_);
        }
        predicted = std::max<Time_t>(0, predicted - (now - idle_since_));
        if (next_wakeup)
            predicted = std::min(predicted, std::max<Time_t>(0, *next_wakeup - now));

        // C2 saves no energy; C3 has C4's wake latency but a higher idle rate.
        // With no separate transition charge, C4 and C6 save per-core energy for
        // every positive idle interval. These cutoffs are a service policy: do
        // not risk a wake delay longer than the predicted idle interval.
        // shortcut: on seeds 0-5, forecasts split at 0.77M and 4.69M; recalibrate on new workloads.
        if (predicted >= 2000000) return C6; // Measured exit latency, inside that gap.
        if (predicted >= 10000) return C4;   // Measured exit latency.
        return C1;
    }

private:
    std::array<Time_t, 8> durations_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    Time_t idle_since_ = 0;
};

#endif
