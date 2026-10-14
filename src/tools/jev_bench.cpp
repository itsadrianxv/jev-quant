#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <string_view>
#include <nlohmann/json.hpp>
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define JEV_HAS_CALLGRIND 1
#else
#define JEV_HAS_CALLGRIND 0
#endif
#include "bench_workload.h"
#include "common/time_utils.h"
#include "trading/order_gw/order_gateway.h"
#include "trading/strategy/jev_worker.h"
#include "trading/strategy/trade_engine.h"

namespace {
using Bench::require;
using Json = nlohmann::json;
const std::array<std::string, 10> scenarios{
    "snapshot-1", "snapshot-5", "snapshot-boundary", "incremental-32",
    "incremental-512", "incremental-8192", "incremental-boundary",
    "pipeline-hold", "pipeline-cancel", "pipeline-fill"};
struct Options {
    std::string scenario = "snapshot-1", mode = "throughput", revision;
    std::filesystem::path out;
    std::size_t updates = 100000, warmup = 1000;
    std::uint64_t seed = 42;
    std::size_t sample_capacity = 0;
};
auto number(std::string_view value) -> std::uint64_t {
    std::uint64_t n = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    require(ec == std::errc{} && end == value.data() + value.size(), "invalid unsigned integer");
    return n;
}
auto options(int argc, char** argv) -> Options {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        require(i + 1 < argc, "missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--scenario") o.scenario = value;
        else if (key == "--mode") o.mode = value;
        else if (key == "--out") o.out = value;
        else if (key == "--revision") o.revision = value;
        else if (key == "--updates") o.updates = number(value);
        else if (key == "--warmup") o.warmup = number(value);
        else if (key == "--seed") o.seed = number(value);
        else if (key == "--sample-capacity") o.sample_capacity = number(value);
        else throw std::runtime_error("unknown option " + key);
    }
    require(std::find(scenarios.begin(), scenarios.end(), o.scenario) != scenarios.end(), "unknown scenario");
    require(o.mode == "throughput" || o.mode == "latency" || o.mode == "stages" || o.mode == "callgrind", "unknown mode");
    require(o.updates > 0 && o.updates <= 1000000 && o.warmup <= 1000000,
            "updates must be 1..1000000 and warmup 0..1000000");
    require(!o.revision.empty() && !o.out.empty(), "--revision and --out are required");
    const auto alignment = o.scenario == "incremental-boundary" ? 8U :
            o.scenario.starts_with("incremental-") || o.scenario == "snapshot-boundary" ? 4U :
            o.scenario == "pipeline-cancel" || o.scenario == "pipeline-fill" ? 2U : 1U;
    require(o.updates % alignment == 0 && o.warmup % alignment == 0,
            "updates and warmup must align to scenario period " + std::to_string(alignment));
    require(o.mode != "stages" || o.scenario.starts_with("pipeline-"), "stages requires a pipeline scenario");
    require(o.sample_capacity <= 4000000, "sample capacity exceeds memory limit");
    return o;
}
struct Sample {
    std::uint64_t ns;
    std::uint32_t stage, kind, requests, request_type;
};
class Collector final : public Trading::ProcessingObserver {
  public:
    explicit Collector(std::size_t capacity) : data(capacity) {}
    std::vector<Sample> data;
    std::size_t used = 0, lost = 0;
    auto push(Sample sample) noexcept -> void {
        if (used == data.size()) { ++lost; return; }
        data[used++] = sample;
    }
    auto record(const Trading::ProcessingSample& s) noexcept -> void override {
        push({s.elapsed_ns, 1U + static_cast<std::uint32_t>(s.stage), s.kind,
              s.request_count, s.request_type});
    }
};
auto distribution(std::vector<std::uint64_t> values) -> Json {
    require(!values.empty(), "empty distribution");
    std::sort(values.begin(), values.end());
    const auto q = [&](double p) { return values[static_cast<std::size_t>(std::ceil(p * values.size())) - 1]; };
    const auto sum = std::accumulate(values.begin(), values.end(), static_cast<long double>(0));
    return {{"count", values.size()}, {"mean_ns", static_cast<double>(sum / values.size())},
            {"p50_ns", q(.50)}, {"p90_ns", q(.90)}, {"p99_ns", q(.99)}, {"max_ns", values.back()}};
}
auto sampleName(const Sample& s) -> std::string {
    if (s.stage == 0) return s.kind == 0 ? "cycle" :
            "book." + Exchange::marketUpdateTypeToString(static_cast<Bench::Type>(s.kind));
    if (s.stage == 1) return "market." + Exchange::marketUpdateTypeToString(static_cast<Bench::Type>(s.kind));
    if (s.stage == 3) return "response." + Exchange::clientResponseTypeToString(static_cast<Exchange::ClientResponseType>(s.kind));
    const std::string intent = s.kind == static_cast<unsigned>(Trading::JevIntent::HOLD) ? "HOLD" :
            s.kind == static_cast<unsigned>(Trading::JevIntent::OPEN) ? "OPEN" : "CLOSE";
    return "decision." + intent + (s.requests ? "." + Exchange::clientRequestTypeToString(
            static_cast<Exchange::ClientRequestType>(s.request_type)) : ".no_request");
}
struct Counts {
    std::uint64_t market = 0, evaluations = 0, decisions = 0, requests = 0;
    std::uint64_t new_orders = 0, cancels = 0, accepted = 0, canceled = 0, fills = 0;
    auto json() const -> Json {
        return {{"market", market}, {"evaluations", evaluations}, {"decisions", decisions},
                {"requests", requests}, {"new", new_orders}, {"cancel", cancels},
                {"accepted", accepted}, {"canceled", canceled}, {"filled", fills}};
    }
};
class ScriptedProvider final : public Trading::JevDecisionProvider {
    const std::string scenario_;
  public:
    explicit ScriptedProvider(std::string scenario) : scenario_(std::move(scenario)) {}
    std::uint64_t evaluated = 0;
    auto evaluate(const Trading::JevEvaluationState& state) -> Trading::JevDecision override {
        ++evaluated;
        Trading::JevDecision d;
        d.evaluation_id_ = state.evaluation_id_;
        d.ticker_id_ = state.ticker_id_;
        d.bias_ = Trading::JevBias::LONG;
        d.intent_ = scenario_ == "pipeline-hold" ? Trading::JevIntent::HOLD :
                scenario_ == "pipeline-fill" && state.position_.net_position_ ?
                Trading::JevIntent::CLOSE : Trading::JevIntent::OPEN;
        return d;
    }
};
class Pipeline {
    Exchange::ClientRequestLFQueue requests_{16};
    Exchange::ClientResponseLFQueue responses_{16};
    Exchange::MarketUpdateLFQueue market_{16};
    Trading::JevDecisionLFQueue decisions_{16};
    Trading::JevEvaluationStateLFQueue evaluations_{16};
    std::string scenario_;
    ScriptedProvider provider_;
    Trading::JevWorker worker_;
    Trading::OrderGateway gateway_;
    std::size_t response_sequence_ = 1;
    std::uint64_t evaluation_id_ = 1;
    bool valid_requests_ = true;
    Exchange::ClientRequest resting_request_;
  public:
    Trading::TradeEngine engine;
    Counts counts;
    explicit Pipeline(const std::string& scenario)
        : scenario_(scenario), provider_(scenario), worker_(&evaluations_, &decisions_, &provider_),
          gateway_(1, &requests_, &responses_),
          engine(1, Trading::RiskLimits{1, 1, 0.0}, &requests_, &responses_, &market_, &decisions_) {
        engine.onMarketUpdate = [this](const auto&, const auto&) { ++counts.market; };
        engine.onJevDecision = [this](const auto&) { ++counts.decisions; };
        engine.onClientResponse = [this](const auto& response) {
            if (response.type_ == Exchange::ClientResponseType::ACCEPTED) ++counts.accepted;
            if (response.type_ == Exchange::ClientResponseType::CANCELED) ++counts.canceled;
            if (response.type_ == Exchange::ClientResponseType::FILLED) ++counts.fills;
        };
        gateway_.setRequestHandler([this](std::size_t, const Exchange::ClientRequest& request) {
            ++counts.requests;
            const bool cancel = request.type_ == Exchange::ClientRequestType::CANCEL;
            if (cancel) ++counts.cancels; else ++counts.new_orders;
            const auto position = engine.positionKeeper().getPositionInfo(0).position_;
            const bool closing = scenario_ == "pipeline-fill" && position != 0;
            const bool expect_cancel = scenario_ == "pipeline-cancel" && counts.market % 2 == 0;
            const auto* bbo = engine.marketOrderBook(0).getBBO();
            valid_requests_ &= request.qty_ == 1 && request.ticker_id_ == 0 &&
                    request.type_ == (expect_cancel ? Exchange::ClientRequestType::CANCEL : Exchange::ClientRequestType::NEW) &&
                    request.price_ == (expect_cancel ? resting_request_.price_ : closing ? bbo->bid_price_ : bbo->ask_price_) &&
                    request.side_ == (closing ? Common::Side::SELL : Common::Side::BUY) &&
                    request.offset_ == (closing ? Common::OrderOffset::CLOSE_TODAY : Common::OrderOffset::OPEN);
            if (cancel) valid_requests_ &= request.order_id_ == resting_request_.order_id_;
            else resting_request_ = request;
            Exchange::ClientResponse r;
            r.type_ = cancel ? Exchange::ClientResponseType::CANCELED : Exchange::ClientResponseType::ACCEPTED;
            r.client_id_ = request.client_id_;
            r.ticker_id_ = request.ticker_id_;
            r.client_order_id_ = request.order_id_;
            r.venue_order_id_ = request.order_id_;
            r.side_ = request.side_;
            r.price_ = request.price_;
            r.exec_qty_ = 0;
            r.leaves_qty_ = cancel ? 0 : request.qty_;
            r.order_type_ = request.order_type_;
            r.offset_ = request.offset_;
            gateway_.publishClientResponse(response_sequence_++, r);
            if (scenario_ == "pipeline-fill") {
                r.type_ = Exchange::ClientResponseType::FILLED;
                r.exec_qty_ = request.qty_;
                r.leaves_qty_ = 0;
                gateway_.publishClientResponse(response_sequence_++, r);
            }
        });
    }
    auto cycle(const Bench::Update& u) -> void {
        *market_.getNextToWriteTo() = u;
        market_.updateWriteIndex();
        engine.processPending();
        const auto id = evaluation_id_++;
        const auto state = engine.buildJevEvaluationState(0, id);
        engine.registerEvaluation(0, id);
        *evaluations_.getNextToWriteTo() = state;
        evaluations_.updateWriteIndex();
        ++counts.evaluations;
        worker_.processPending();
        engine.processPending();
        gateway_.processPending();
        engine.processPending();
    }
    auto checksum(std::uint64_t hash) -> std::uint64_t {
        hash = Bench::digest(hash, *engine.marketOrderBook(0).getBBO());
        const auto& p = engine.positionKeeper().getPositionInfo(0);
        hash = Bench::mix(hash, static_cast<std::uint64_t>(p.position_));
        hash = Bench::mix(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(p.realized_pnl_)));
        for (auto side : {Common::Side::BUY, Common::Side::SELL}) {
            const auto& order = engine.orderManager().getOrder(0, side);
            hash = Bench::mix(hash, static_cast<std::uint64_t>(order.order_state_));
            hash = Bench::mix(hash, order.order_id_);
        }
        return hash;
    }
    auto validate(std::size_t completed, const Trading::BBO& expected,
                  const Exchange::DepthSnapshot& snapshot) -> void {
        require(valid_requests_, "invalid request fields");
        require(worker_.failureCount() == 0, "provider failure");
        require(provider_.evaluated == completed, "provider evaluation count mismatch");
        require(requests_.size() + responses_.size() + market_.size() + decisions_.size() + evaluations_.size() == 0,
                "pipeline queues not empty");
        require(Bench::equal(*engine.marketOrderBook(0).getBBO(), expected), "pipeline BBO mismatch");
        require(Bench::snapshotEqual(*engine.marketOrderBook(0).getDepthSnapshot(), snapshot), "pipeline snapshot mismatch");
        const auto& p = engine.positionKeeper().getPositionInfo(0);
        const bool fill = scenario_ == "pipeline-fill", cancel = scenario_ == "pipeline-cancel";
        require(p.position_ == (fill ? static_cast<int>(completed % 2) : 0) &&
                p.todayQty() == (fill ? completed % 2 : 0) && p.yesterdayQty() == 0, "position mismatch");
        require(p.realized_pnl_ == (fill ? -static_cast<double>(completed / 2) : 0.0), "realized PnL mismatch");
        using State = Trading::OMOrderState;
        const auto buy = engine.orderManager().getOrder(0, Common::Side::BUY).order_state_;
        const auto sell = engine.orderManager().getOrder(0, Common::Side::SELL).order_state_;
        require(buy == (fill ? State::DEAD : cancel ? (completed % 2 ? State::LIVE : State::DEAD) : State::INVALID),
                "buy state mismatch");
        require(sell == (fill && completed >= 2 ? State::DEAD : State::INVALID), "sell state mismatch");
    }
};
auto profile(bool start) -> void {
#if JEV_HAS_CALLGRIND
    if (start) { CALLGRIND_ZERO_STATS; CALLGRIND_TOGGLE_COLLECT; }
    else { CALLGRIND_TOGGLE_COLLECT; CALLGRIND_DUMP_STATS_AT("measured-batch"); }
#else
    (void)start;
    throw std::runtime_error("Callgrind headers unavailable at build time");
#endif
}
auto run(const Options& o) -> Json {
    const bool pipeline = o.scenario.starts_with("pipeline-");
    if (o.mode == "callgrind") {
#if JEV_HAS_CALLGRIND
        require(RUNNING_ON_VALGRIND, "callgrind mode requires valgrind --tool=callgrind --collect-atstart=no");
#else
        throw std::runtime_error("Callgrind headers unavailable at build time");
#endif
    }
    auto workload = Bench::generate(o.scenario, o.warmup + o.updates, o.seed);
    std::uint64_t expected_hash = Bench::kHashStart;
    // Verify every intermediate result in an untimed preflight.
    if (pipeline) {
        Pipeline check(o.scenario);
        for (std::size_t i = 0; i < workload.events.size(); ++i) {
            check.cycle(workload.events[i]);
            check.validate(i + 1, workload.expected[i], workload.events[i].depth_snapshot_);
            if (i >= o.warmup) expected_hash = check.checksum(expected_hash);
        }
    } else {
        Trading::MarketOrderBook check(0);
        for (const auto& u : workload.setup) check.onMarketUpdate(u);
        for (std::size_t i = 0; i < workload.events.size(); ++i) {
            check.onMarketUpdate(workload.events[i]);
            require(Bench::equal(*check.getBBO(), workload.expected[i]), "reference BBO mismatch at " + std::to_string(i));
            if (workload.events[i].type_ == Bench::Type::DEPTH_SNAPSHOT)
                require(Bench::snapshotEqual(*check.getDepthSnapshot(), workload.events[i].depth_snapshot_), "full snapshot mismatch");
            if (i >= o.warmup) {
                expected_hash = Bench::digest(expected_hash, workload.expected[i]);
                if (workload.events[i].type_ == Bench::Type::DEPTH_SNAPSHOT)
                    expected_hash = Bench::snapshotDigest(expected_hash, workload.events[i].depth_snapshot_);
            }
        }
    }
    const auto expected_samples = o.mode == "latency" ? o.updates : o.mode == "stages" ?
            o.updates * (o.scenario == "pipeline-fill" ? 4 : o.scenario == "pipeline-cancel" ? 3 : 2) : 0;
    Collector samples(o.sample_capacity ? o.sample_capacity : expected_samples);
    std::unique_ptr<Pipeline> chain;
    std::unique_ptr<Trading::MarketOrderBook> book;
    if (pipeline) chain = std::make_unique<Pipeline>(o.scenario);
    else {
        book = std::make_unique<Trading::MarketOrderBook>(0);
        for (const auto& u : workload.setup) book->onMarketUpdate(u);
    }
    const auto operation = [&](std::size_t i) {
        if (pipeline) chain->cycle(workload.events[i]);
        else book->onMarketUpdate(workload.events[i]);
    };
    for (std::size_t i = 0; i < o.warmup; ++i) operation(i);
    if (pipeline) {
        chain->counts = {};
        if (o.mode == "stages") chain->engine.setProcessingObserver(&samples);
    }
    std::uint64_t checksum = Bench::kHashStart;
    if (o.mode == "callgrind") profile(true);
    const auto batch_start = Common::nowSteadyNs();
    for (std::size_t i = o.warmup; i < workload.events.size(); ++i) {
        const auto start = o.mode == "latency" ? Common::nowSteadyNs() : 0;
        operation(i);
        if (o.mode == "latency") samples.push({Common::nowSteadyNs() - start, 0,
                pipeline ? 0U : static_cast<std::uint32_t>(workload.events[i].type_), 0, 0});
        // Consumption cost is included in batch/profile, outside the individual
        // interval. Reading every result prevents dead-store elimination.
        checksum = pipeline ? chain->checksum(checksum) : Bench::digest(checksum, *book->getBBO());
        if (!pipeline && workload.events[i].type_ == Bench::Type::DEPTH_SNAPSHOT)
            checksum = Bench::snapshotDigest(checksum, *book->getDepthSnapshot());
    }
    const auto batch_ns = Common::nowSteadyNs() - batch_start;
    if (o.mode == "callgrind") profile(false);
    if (pipeline) chain->engine.setProcessingObserver(nullptr);
    require(checksum == expected_hash, "measured output checksum mismatch");
    require(samples.lost == 0 && samples.used == expected_samples, "sample completeness failure");
    Json counts;
    if (pipeline) {
        chain->validate(workload.events.size(), workload.expected.back(), workload.events.back().depth_snapshot_);
        const auto& c = chain->counts;
        const auto n = o.updates;
        require(c.market == n && c.evaluations == n && c.decisions == n, "input/evaluation count mismatch");
        const auto new_expected = o.scenario == "pipeline-fill" ? n : o.scenario == "pipeline-cancel" ? n / 2 : 0;
        const auto cancel_expected = o.scenario == "pipeline-cancel" ? n / 2 : 0;
        require(c.new_orders == new_expected && c.accepted == new_expected &&
                c.cancels == cancel_expected && c.canceled == cancel_expected &&
                c.fills == (o.scenario == "pipeline-fill" ? n : 0) &&
                c.requests == new_expected + cancel_expected, "request/response count mismatch");
        counts = c.json();
    } else {
        counts = Json::object();
        for (std::size_t i = o.warmup; i < workload.events.size(); ++i) {
            const auto name = Exchange::marketUpdateTypeToString(workload.events[i].type_);
            counts[name] = counts.value(name, 0ULL) + 1;
        }
    }
    Json result{{"schema_version", 1}, {"valid", true}, {"scenario", o.scenario}, {"mode", o.mode},
                {"revision", o.revision}, {"compiler", __VERSION__}, {"seed", o.seed},
                {"warmup", o.warmup}, {"completed", o.updates}, {"unit", pipeline ? "cycle" : "update"},
                {"workload_hash64", std::to_string(workload.hash)},
                {"result_checksum", std::to_string(checksum)}, {"counts", counts},
                {"samples_expected", expected_samples}, {"samples_recorded", samples.used},
                {"callgrind_available", static_cast<bool>(JEV_HAS_CALLGRIND)}};
    if (o.mode == "throughput") {
        require(batch_ns > 0, "zero batch time");
        result["batch_ns"] = batch_ns;
        result["ns_per_operation"] = static_cast<double>(batch_ns) / o.updates;
        result["operations_per_second"] = 1e9 * o.updates / batch_ns;
    }
    std::map<std::string, std::vector<std::uint64_t>> groups;
    for (std::size_t i = 0; i < samples.used; ++i) groups[sampleName(samples.data[i])].push_back(samples.data[i].ns);
    if (o.mode == "stages") {
        std::map<std::string, std::size_t> expected{{"market.DEPTH_SNAPSHOT", o.updates}};
        if (o.scenario == "pipeline-hold") expected["decision.HOLD.no_request"] = o.updates;
        if (o.scenario == "pipeline-cancel") {
            for (const auto* name : {"decision.OPEN.NEW", "decision.OPEN.CANCEL", "response.ACCEPTED", "response.CANCELED"})
                expected[name] = o.updates / 2;
        }
        if (o.scenario == "pipeline-fill") {
            expected["decision.OPEN.NEW"] = expected["decision.CLOSE.NEW"] = o.updates / 2;
            expected["response.ACCEPTED"] = expected["response.FILLED"] = o.updates;
        }
        require(groups.size() == expected.size(), "unexpected observation category");
        for (const auto& [name, count] : expected)
            require(groups.contains(name) && groups.at(name).size() == count, "observation category count mismatch: " + name);
        for (std::size_t i = 0; i < samples.used; ++i) {
            const auto& s = samples.data[i];
            require(s.stage != 2 || s.requests == (o.scenario == "pipeline-hold" ? 0U : 1U), "decision request observation mismatch");
        }
    }
    result["distributions"] = Json::object();
    for (auto& [name, values] : groups) result["distributions"][name] = distribution(std::move(values));
    if (expected_samples) {
        Collector control(4096);
        std::vector<std::uint64_t> clock_pairs(4096);
        const auto control_start = Common::nowSteadyNs();
        for (std::size_t i = 0; i < clock_pairs.size(); ++i) {
            const auto t = Common::nowSteadyNs();
            const auto elapsed = Common::nowSteadyNs() - t;
            control.push({elapsed, 0, 0, 0, 0});
        }
        const auto control_ns = Common::nowSteadyNs() - control_start;
        for (std::size_t i = 0; i < clock_pairs.size(); ++i) clock_pairs[i] = control.data[i].ns;
        result["clock_pair_control"] = distribution(std::move(clock_pairs));
        result["clock_pair_and_record_mean_ns"] = static_cast<double>(control_ns) / control.used;
    }
    std::filesystem::create_directories(o.out);
    require(std::filesystem::is_empty(o.out), "output directory must be empty");
    const auto write = [&](const std::string& name, const auto& callback) {
        std::ofstream stream(o.out / name);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        callback(stream);
        stream.close();
    };
    if (expected_samples) write("samples.csv", [&](auto& stream) {
        stream << "index,metric,elapsed_ns,request_count\n";
        for (std::size_t i = 0; i < samples.used; ++i) {
            const auto& s = samples.data[i];
            stream << i << ',' << sampleName(s) << ',' << s.ns << ',' << s.requests << '\n';
        }
    });
    write("summary.json", [&](auto& stream) { stream << result.dump(2) << '\n'; });
    std::cout << result.dump(2) << '\n';
    return result;
}
}  // namespace
int main(int argc, char** argv) {
    try { run(options(argc, argv)); return 0; }
    catch (const std::exception& e) { std::cerr << "jev_bench: " << e.what() << '\n'; return 1; }
}
