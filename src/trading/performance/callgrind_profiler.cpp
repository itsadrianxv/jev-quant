#include "callgrind_profiler.h"

#include <limits>
#include <stdexcept>

#ifdef JEV_HAVE_CALLGRIND
#include <valgrind/callgrind.h>
#endif

namespace Trading {

// Validate the standalone Callgrind collection window.
CallgrindProfiler::CallgrindProfiler(std::uint64_t window_ns) : window_ns_(window_ns) {
    if (!window_ns_ || window_ns_ > std::numeric_limits<std::uint64_t>::max() - 1)
        throw std::invalid_argument("Invalid Callgrind profiling window");
}

// Arm collection at the supplied readiness timestamp.
void CallgrindProfiler::startCapture(std::uint64_t now) {
    if (!now || started_.load(std::memory_order_acquire))
        throw std::logic_error("Callgrind capture can only be armed once");
    start_.store(now, std::memory_order_release);
    end_.store(now + window_ns_, std::memory_order_release);
    started_.store(true, std::memory_order_release);
}

// Prevent new scopes from opening after the explicit stop timestamp.
void CallgrindProfiler::stopCapture(std::uint64_t now) noexcept {
    end_.store(now, std::memory_order_release);
}

// Create a disabled or enabled RAII collection guard for the current engine event.
CallgrindProfiler::Scope CallgrindProfiler::collect() noexcept {
    const auto now = Common::nowSteadyNs();
    const auto start = start_.load(std::memory_order_acquire);
    const auto end = end_.load(std::memory_order_acquire);
    return Scope(started_.load(std::memory_order_acquire) && start && end && now >= start && now < end);
}

// Collect instructions for one market event while the profiling window is open.
CallgrindProfiler::Scope CallgrindProfiler::collectMarket() noexcept {
    return collect();
}

// Collect instructions for one decision event while the profiling window is open.
CallgrindProfiler::Scope CallgrindProfiler::collectDecision() noexcept {
    return collect();
}

// Toggle collection when an eligible scope begins.
CallgrindProfiler::Scope::Scope(bool enabled) noexcept : enabled_(enabled) {
#ifdef JEV_HAVE_CALLGRIND
    if (enabled_) CALLGRIND_TOGGLE_COLLECT;
#else
    (void)enabled_;
#endif
}

// Restore the previous Callgrind collection state when the scope ends.
CallgrindProfiler::Scope::~Scope() {
#ifdef JEV_HAVE_CALLGRIND
    if (enabled_) CALLGRIND_TOGGLE_COLLECT;
#endif
}

}  // namespace Trading
