#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>
#include "common/performance_trace.h"
#include "common/time_utils.h"
#include "exchange/order_server/client_request.h"

namespace Trading {
// One run, armed once by main after venue readiness. Source IDs belong to the
// CTP market callback; sample storage belongs exclusively to the gateway worker.
// Read/export only after both workers have joined.
class Measurement final {
public:
    struct Sample {
        Common::PerformanceTrace trace;
        Common::OrderId order_id;
        Common::TickerId ticker_id;
        Exchange::ClientRequestType type;
        Common::OrderOffset offset;
    };
    Measurement(std::size_t capacity, std::uint64_t window_ns,
                std::uint64_t grace_ns, bool callgrind);
    void startCapture(std::uint64_t now = Common::nowSteadyNs());
    void finish(std::uint64_t now = Common::nowSteadyNs()) noexcept { end_.store(now, std::memory_order_release); }
    bool admitting(std::uint64_t now) const noexcept;
    bool collecting(std::uint64_t now) const noexcept;
    Common::PerformanceTrace source(std::uint64_t now) noexcept;
    void record(const Exchange::ClientRequest&, int result) noexcept;
    void write(const std::string& directory, bool healthy,
               std::uint64_t provider_failures) const;
    bool callgrind() const noexcept { return callgrind_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t overflow() const noexcept { return overflow_; }
    const Sample& sample(std::size_t i) const { return samples_.at(i); }
    // Engine-thread counters, inspected only after join; no per-event timing.
    void countProfile(bool decision) noexcept { decision ? ++profile_decisions_ : ++profile_markets_; }
private:
    std::vector<Sample> samples_;
    const std::uint64_t window_ns_, grace_ns_;
    const bool callgrind_;
    std::atomic<std::uint64_t> start_{0};
    std::atomic<std::uint64_t> end_{0};
    std::uint64_t next_source_ = 0;
    std::size_t size_ = 0, overflow_ = 0;
    std::uint64_t profile_markets_ = 0, profile_decisions_ = 0;
};

// Callgrind collection is thread-local with --separate-threads=yes. Start with
// --collect-atstart=no. Instrumentation remains on throughout the process.
class InstructionScope final {
public:
    explicit InstructionScope(Measurement* measurement, bool decision = false);
    ~InstructionScope();
    InstructionScope(const InstructionScope&) = delete;
    InstructionScope& operator=(const InstructionScope&) = delete;
private:
    bool enabled_;
};
}
