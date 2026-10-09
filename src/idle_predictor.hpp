#ifndef IDLE_PREDICTOR_HPP
#define IDLE_PREDICTOR_HPP

#include "sim_types.h"

#include <algorithm>
#include <array>
#include <optional>

#ifndef EEC_IDLE_INITIAL_GRACE
#define EEC_IDLE_INITIAL_GRACE 1000
#endif

class IdlePredictor {
public:
    // Called when the CPU becomes idle.
    void Begin(Time_t now) {
        idle_since_ = now;
        observing_ = true;
    }

    // Called when work requires the CPU again. This is deliberately separate
    // from wake completion so transition latency is not learned as idle time.
    void End(Time_t now) {
        if (!observing_)
            return;
        observing_ = false;

        const Time_t duration = std::max<Time_t>(0, now - idle_since_);
        if (duration == 0)
            return;
        durations_[next_] = duration;
        next_ = (next_ + 1) % durations_.size();
        count_ = std::min(count_ + 1, durations_.size());
    }

    // Returns the recommended C-state the CPU should enter at this time.
    CState_t Select(Time_t now, std::optional<Time_t> next_wakeup = std::nullopt) const {
        static_assert(EEC_IDLE_INITIAL_GRACE >= 0,
                      "Initial idle grace period must be nonnegative");

        if (!observing_)
            return C1;

        const Time_t elapsed = std::max<Time_t>(0, now - idle_since_);
        // CreateProcess callbacks for a burst can share one timestamp. Keep an
        // untrained core loadable until that burst has been admitted instead of
        // forcing an avoidable C4 wake after the first process in the burst.
        if (count_ == 0 && elapsed < EEC_IDLE_INITIAL_GRACE)
            return C1;

        Time_t predicted = 20000;
        if (count_) {
            predicted = 0;
            Time_t remainder = 0;
            for (std::size_t i = 0; i < count_; ++i) {
                predicted += durations_[i] / static_cast<Time_t>(count_);
                remainder += durations_[i] % static_cast<Time_t>(count_);
            }
            predicted += remainder / static_cast<Time_t>(count_);
        }
        predicted = std::max<Time_t>(0, predicted - elapsed);
        if (next_wakeup)
            predicted = std::min(predicted, std::max<Time_t>(0, *next_wakeup - now));

        // C2 saves no energy; C3 has C4's wake latency at a higher idle rate.
        if (predicted >= 2000000) return C6;
        if (predicted >= 10000) return C4;
        return C1;
    }

private:
    std::array<Time_t, 8> durations_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    Time_t idle_since_ = 0;
    bool observing_ = false;
};

#endif
