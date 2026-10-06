#ifndef JOB_PLACEMENT_HPP
#define JOB_PLACEMENT_HPP

#include "sim_types.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

// Only initial work from admitted jobs is observed. Completed jobs stay in
// the histogram; execution and queue occupancy never change the distribution.
class WorkDistribution {
public:
    double Percentile(Time_t work) const {
        if (count == 0)
            return 0.5;
        std::uint64_t less = 0, equal = 0;
        for (const auto &entry : histogram) {
            if (entry.first > work)
                break;
            if (entry.first == work)
                equal = entry.second;
            else
                less += entry.second;
        }
        return (static_cast<double>(less) + 0.5 * equal) / count;
    }

    void Observe(Time_t work) {
        if (work < 0)
            throw std::invalid_argument("Work units must be nonnegative");
        ++histogram[work];
        ++count;
        // Interpolated order statistics at (N - 1) * q, including endpoints.
        for (std::size_t i = 0; i < quantiles.size(); ++i) {
            const double position = static_cast<double>(count - 1) * i / 4.0;
            const auto lower = static_cast<std::uint64_t>(position);
            const double fraction = position - lower;
            const double first = At(lower);
            quantiles[i] = first + fraction * (At(lower + (fraction > 0)) - first);
        }
    }

    std::uint64_t Count() const { return count; }
    std::optional<std::array<double, 5>> Summary() const {
        if (count == 0)
            return std::nullopt;
        return quantiles;
    }

private:
    double At(std::uint64_t position) const {
        for (const auto &entry : histogram) {
            if (position < entry.second)
                return static_cast<double>(entry.first);
            position -= entry.second;
        }
        throw std::out_of_range("Work distribution order statistic");
    }

    std::map<Time_t, std::uint64_t> histogram;
    std::uint64_t count = 0;
    std::array<double, 5> quantiles{};
};

class PlacementPolicy {
public:
    using Curve = std::array<double, 5>;
    static constexpr Curve DefaultCurve = {0, 0.25, 0.5, 0.75, 1};
    struct Decision {
        double percentile;
        double high_probability; // Effective probability after capacity fallback.
        bool high;
    };

    explicit PlacementPolicy(Curve curve = DefaultCurve,
                             std::uint32_t seed = 0)
        : probabilities(curve), seed(seed), random(seed) {
        for (std::size_t i = 0; i < probabilities.size(); ++i) {
            if (!std::isfinite(probabilities[i]) || probabilities[i] < 0 ||
                probabilities[i] > 1 || (i && probabilities[i] < probabilities[i - 1]))
                throw std::invalid_argument("High placement probabilities must be finite, "
                                            "nondecreasing values in [0, 1]");
        }
    }

    static PlacementPolicy FromEnvironment() {
        Curve curve = DefaultCurve;
        std::uint32_t seed = 0;
        if (const char *value = std::getenv("EEC_HIGH_PROBABILITIES")) {
            std::istringstream input(value);
            for (std::size_t i = 0; i < curve.size(); ++i) {
                char separator = 0;
                if (!(input >> curve[i]) ||
                    (i + 1 < curve.size() && (!(input >> separator) || separator != ',')))
                    throw std::invalid_argument("EEC_HIGH_PROBABILITIES requires five "
                                                "comma-separated probabilities");
            }
            input >> std::ws;
            if (!input.eof())
                throw std::invalid_argument("Unexpected text after EEC_HIGH_PROBABILITIES");
        }
        if (const char *value = std::getenv("EEC_PLACEMENT_SEED")) {
            const std::string text(value);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), seed);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
                throw std::invalid_argument("EEC_PLACEMENT_SEED must be a uint32 integer");
        }
        return PlacementPolicy(curve, seed);
    }

    double HighProbability(double percentile) const {
        const double position = std::clamp(percentile, 0.0, 1.0) * 4;
        const auto lower = std::min(static_cast<std::size_t>(position), std::size_t{3});
        return probabilities[lower] + (position - lower) *
               (probabilities[lower + 1] - probabilities[lower]);
    }

    Decision Admit(Time_t work, WorkDistribution &distribution,
                   bool high_enabled, bool low_enabled) {
        if (!high_enabled && !low_enabled)
            throw std::invalid_argument("Placement requires an enabled core type");
        const double percentile = distribution.Percentile(work);
        const double probability = !high_enabled ? 0 : !low_enabled ? 1 :
                                   HighProbability(percentile);
        distribution.Observe(work);
        // One dedicated draw per admission, including forced placements. This
        // preserves paired draws and does not consume the workload's RNG.
        const double draw = static_cast<double>(random()) / 4294967296.0;
        return {percentile, probability, draw < probability};
    }

    const Curve &Probabilities() const { return probabilities; }
    std::uint32_t Seed() const { return seed; }

private:
    Curve probabilities;
    std::uint32_t seed;
    std::mt19937 random;
};

#endif
