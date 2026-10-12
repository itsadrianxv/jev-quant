#pragma once

#include <chrono>
#include <cstdint>

namespace Common {

/// Monotonic nanosecond timestamp source for latency measurement.
///
/// Measurement math must never use a wall clock: adjustments would corrupt
/// durations. The steady clock is sufficient while measured latencies are in
/// the microsecond-to-second range.
// TODO: Switch to a TSC-based source with frequency calibration when
// sub-microsecond resolution becomes necessary.
[[nodiscard]] inline auto nowSteadyNs() noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
}

}  // namespace Common
