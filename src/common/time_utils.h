#pragma once

#include <chrono>
#include <cstdint>

namespace Common {

/// Monotonic nanosecond timestamp source for latency measurement.
///
/// Measurement math must never use a wall clock: adjustments would corrupt
/// durations. Report clock/recorder overhead separately for short operations.
// TODO: Evaluate a calibrated TSC source only if clock overhead limits a study.
[[nodiscard]] inline auto nowSteadyNs() noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace Common
