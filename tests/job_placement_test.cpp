#include "job_placement.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void Require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename F> void Reject(F operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    Require(rejected, "Invalid placement input was accepted");
}
}

int main() {
    try {
        WorkDistribution distribution;
        Require(!distribution.Summary() && distribution.Percentile(20) == 0.5,
                "Empty distribution must use a neutral rank and no quantiles");
        Reject([&] { distribution.Observe(-1); });
        Require(distribution.Count() == 0, "Invalid work changed the histogram");
        distribution.Observe(10);
        Require(*distribution.Summary() == std::array<double, 5>{10, 10, 10, 10, 10},
                "Single sample quantiles are incorrect");
        for (Time_t work : {20, 20, 40})
            distribution.Observe(work);
        Require(*distribution.Summary() == std::array<double, 5>{10, 17.5, 20, 25, 40},
                "Interpolated quantiles are incorrect");
        Require(distribution.Percentile(0) == 0 && distribution.Percentile(50) == 1 &&
                distribution.Percentile(20) == 0.5 && distribution.Percentile(25) == 0.75,
                "Ranks must handle extrema, ties, and gaps");
        WorkDistribution equal;
        for (int i = 0; i < 100; ++i)
            equal.Observe(0);
        Require(equal.Percentile(0) == 0.5, "Equal jobs must have neutral ranks");

        PlacementPolicy linear({0, 0.25, 0.5, 0.75, 1}, 17);
        for (double rank : {0.0, 0.125, 0.25, 0.5, 0.625, 0.75, 1.0})
            Require(linear.HighProbability(rank) == rank, "Curve interpolation failed");
        Reject([] { PlacementPolicy policy({0, 0.5, 0.4, 0.7, 1}); });
        Reject([] { PlacementPolicy policy({-0.1, 0, 0, 0, 1}); });
        Reject([] { PlacementPolicy policy({0, 0, 0, 0, 1.1}); });
        Reject([] { PlacementPolicy policy({0, 0, 0, 0, std::numeric_limits<double>::quiet_NaN()}); });

        PlacementPolicy first, second;
        WorkDistribution first_distribution, second_distribution;
        for (Time_t work : {10, 20, 20, 0, 40, 15}) {
            const auto left = first.Admit(work, first_distribution, true, true);
            // Workload RNG activity must have no effect on placement replay.
            std::srand(static_cast<unsigned>(work));
            for (int i = 0; i < 10; ++i)
                (void)std::rand();
            const auto right = second.Admit(work, second_distribution, true, true);
            Require(left.high == right.high && left.percentile == right.percentile &&
                    left.high_probability == right.high_probability, "Seeded replay failed");
        }
        WorkDistribution fresh;
        PlacementPolicy policy;
        const auto admission = policy.Admit(100, fresh, true, true);
        Require(admission.percentile == 0.5 && admission.high_probability == 0.5,
                "First admission must rank against prior observations");
        Require(policy.Admit(200, fresh, false, true).high_probability == 0 &&
                policy.Admit(1, fresh, true, false).high_probability == 1,
                "Disabled core types must force placement to available capacity");
        Reject([&] { policy.Admit(1, fresh, false, false); });

        PlacementPolicy always_low({0, 0, 0, 0, 0});
        PlacementPolicy always_high({1, 1, 1, 1, 1});
        WorkDistribution low, high;
        for (int i = 0; i < 1000; ++i) {
            Require(!always_low.Admit(i, low, true, true).high, "Zero probability chose high");
            Require(always_high.Admit(i, high, true, true).high, "Unit probability chose low");
        }
        PlacementPolicy balanced({0.5, 0.5, 0.5, 0.5, 0.5}, 123);
        WorkDistribution balanced_distribution;
        int high_count = 0;
        for (int i = 0; i < 20000; ++i)
            high_count += balanced.Admit(10, balanced_distribution, true, true).high;
        Require(high_count > 9600 && high_count < 10400, "Random placement is biased");

        unsetenv("EEC_HIGH_PROBABILITIES");
        unsetenv("EEC_PLACEMENT_SEED");
        const auto defaults = PlacementPolicy::FromEnvironment();
        Require(defaults.Seed() == 0 &&
                defaults.Probabilities() == PlacementPolicy::Curve{0, 0.25, 0.5, 0.75, 1},
                "Default configuration must use the linear curve and seed zero");
        setenv("EEC_HIGH_PROBABILITIES", "0, 0.25, 0.5, 0.75, 1", 1);
        setenv("EEC_PLACEMENT_SEED", "4294967295", 1);
        const auto configured = PlacementPolicy::FromEnvironment();
        Require(configured.HighProbability(0.125) == 0.125 &&
                configured.Seed() == 4294967295U, "Environment configuration failed");
        for (const char *seed : {"", "-1", "4294967296", "3junk"}) {
            setenv("EEC_PLACEMENT_SEED", seed, 1);
            Reject([] { PlacementPolicy::FromEnvironment(); });
        }
        unsetenv("EEC_PLACEMENT_SEED");
        for (const char *curve : {"", "0,0,0,0", "0,0,0,0,0,0", "0,0,0,0,0junk",
                                  "0,0.5,0.4,0.8,1", "0,0,0,0,2", "nan,0,0,0,1"}) {
            setenv("EEC_HIGH_PROBABILITIES", curve, 1);
            Reject([] { PlacementPolicy::FromEnvironment(); });
        }
        std::cout << "Placement distribution, policy, RNG, and configuration checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << "PLACEMENT TEST FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
