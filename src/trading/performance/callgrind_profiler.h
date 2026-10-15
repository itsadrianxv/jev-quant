#pragma once

#include <cstdint>
#include <atomic>

#include "common/time_utils.h"

namespace Trading {

// Own the engine-thread Callgrind collection window and its short-lived scopes.
class CallgrindProfiler final {
public:
    explicit CallgrindProfiler(std::uint64_t window_ns);

    void startCapture(std::uint64_t now = Common::nowSteadyNs());
    void stopCapture(std::uint64_t now = Common::nowSteadyNs()) noexcept;

    class Scope final {
    public:
        explicit Scope(bool enabled) noexcept;
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        bool enabled_;
    };

    Scope collectMarket() noexcept;
    Scope collectDecision() noexcept;

private:
    Scope collect() noexcept;

    const std::uint64_t window_ns_;
    std::atomic<std::uint64_t> start_{0};
    std::atomic<std::uint64_t> end_{0};
    std::atomic<bool> started_{false};
};

}  // namespace Trading
