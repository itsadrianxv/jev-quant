#include "measurement.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <nlohmann/json.hpp>
#ifdef JEV_HAVE_CALLGRIND
#include <valgrind/callgrind.h>
#endif

namespace Trading {
Measurement::Measurement(std::size_t capacity, std::uint64_t window_ns,
                         std::uint64_t grace_ns, bool callgrind)
    : samples_(capacity), window_ns_(window_ns), grace_ns_(grace_ns), callgrind_(callgrind) {
    if (!capacity || !window_ns || !grace_ns ||
        window_ns > std::numeric_limits<std::uint64_t>::max() - grace_ns)
        throw std::invalid_argument("Invalid performance capture bounds");
#ifndef JEV_HAVE_CALLGRIND
    if (callgrind) throw std::runtime_error("Rebuild with Valgrind development headers for Callgrind capture");
#else
    if (callgrind && !RUNNING_ON_VALGRIND)
        throw std::runtime_error("Callgrind capture must run under Valgrind; use run_simnow.py");
#endif
}
void Measurement::startCapture(std::uint64_t now) {
    std::uint64_t expected = 0;
    if (!now || !start_.compare_exchange_strong(expected, now))
        throw std::logic_error("Performance capture can only be armed once");
}
bool Measurement::admitting(std::uint64_t now) const noexcept {
    const auto start = start_.load(std::memory_order_acquire);
    const auto end = end_.load(std::memory_order_acquire);
    return start && (!end || now < end) && now >= start && now - start < window_ns_;
}
bool Measurement::collecting(std::uint64_t now) const noexcept {
    const auto start = start_.load(std::memory_order_acquire);
    const auto end = end_.load(std::memory_order_acquire);
    return start && (!end || now < end) && now >= start && now - start < window_ns_ + grace_ns_;
}
Common::PerformanceTrace Measurement::source(std::uint64_t now) noexcept {
    Common::PerformanceTrace trace;
    if (admitting(now)) { trace.t[0] = now; trace.source_id = ++next_source_; }
    return trace;
}
void Measurement::record(const Exchange::ClientRequest& request, int result) noexcept {
    const auto& trace = request.trace_;
    if (result || !collecting(trace.t[7])) return;
    const bool cancel = request.type_ == Exchange::ClientRequestType::CANCEL;
    if (!cancel && (request.type_ != Exchange::ClientRequestType::NEW ||
                    !trace.source_id || !trace.evaluation_id || !admitting(trace.t[0]))) return;
    const std::size_t first = cancel ? 4 : 0;
    if (cancel && !admitting(trace.t[4])) return;
    for (std::size_t i = first; i < 8; ++i)
        if (!trace.t[i] || (i > first && trace.t[i] < trace.t[i-1])) return;
    if (size_ == samples_.size()) { ++overflow_; return; }
    samples_[size_++] = {trace, request.order_id_, request.ticker_id_, request.type_, request.offset_};
}
void Measurement::write(const std::string& directory, bool healthy, std::uint64_t failures) const {
    namespace fs = std::filesystem;
    fs::create_directories(directory);
    const auto csv_path = fs::path(directory) / "samples.csv";
    const auto meta_path = fs::path(directory) / "capture.json";
    if (fs::exists(csv_path) || fs::exists(meta_path))
        throw std::runtime_error("Performance output already exists; choose a new directory");
    std::ofstream csv(csv_path);
    csv.exceptions(std::ios::badbit | std::ios::failbit);
    csv << "type,offset,order_id,ticker_id,source_id,evaluation_id,t1,t2,t3,t4,t5,t6,t7,t8\n";
    std::size_t new_count = 0;
    for (std::size_t i=0; i<size_; ++i) {
        const auto& s = samples_[i];
        new_count += s.type == Exchange::ClientRequestType::NEW;
        csv << Exchange::clientRequestTypeToString(s.type) << ',' << Common::orderOffsetToString(s.offset)
            << ',' << s.order_id << ',' << s.ticker_id << ',' << s.trace.source_id << ',' << s.trace.evaluation_id;
        for (auto t : s.trace.t) csv << ',' << t;
        csv << '\n';
    }
    csv.close();
    const auto now = Common::nowSteadyNs();
    const auto start = start_.load();
    const auto end = end_.load();
    const auto completed_at = end ? end : now;
    nlohmann::json meta{{"schema", 1}, {"mode", callgrind_ ? "callgrind" : "native"},
        {"clock", "steady_clock_nanoseconds"}, {"start_ns", start}, {"end_ns", end}, {"export_ns", now},
        {"window_ns", window_ns_}, {"grace_ns", grace_ns_}, {"capacity", samples_.size()},
        {"samples", size_}, {"new_samples", new_count}, {"overflow", overflow_},
        {"healthy", healthy}, {"provider_failures", failures},
        {"profile_market_events", profile_markets_}, {"profile_decision_events", profile_decisions_},
        {"window_completed", start && completed_at >= start && completed_at-start >= window_ns_+grace_ns_},
        {"live_acceptance", "pending_manual_evidence_review"}};
    std::ofstream out(meta_path);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << meta.dump(2) << '\n';
    out.close();
}
InstructionScope::InstructionScope(Measurement* m, bool decision)
    : enabled_(m && m->callgrind() && m->admitting(Common::nowSteadyNs())) {
    if (enabled_) m->countProfile(decision);
#ifdef JEV_HAVE_CALLGRIND
    if (enabled_) { CALLGRIND_TOGGLE_COLLECT; }
#endif
}
InstructionScope::~InstructionScope() {
#ifdef JEV_HAVE_CALLGRIND
    if (enabled_) { CALLGRIND_TOGGLE_COLLECT; }
#endif
}
}
