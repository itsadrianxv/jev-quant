#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/async_logger.h"
#include "common/time_utils.h"
#include "trading/order_gw/order_gateway.h"
#include "trading/strategy/jev_worker.h"
#include "trading/strategy/trade_engine.h"

namespace {

constexpr std::uint64_t kDefaultUpdates = 100000;
constexpr std::size_t kQueueCapacity = 1024;
constexpr std::size_t kLoggerQueueCapacity = 16384;

struct Options {
    std::uint64_t updates = kDefaultUpdates;
    std::filesystem::path out_dir = std::filesystem::temp_directory_path() /
                                                                    "jev_quant_bench_ttt";
};

auto parseOptions(int argc, char** argv) -> Options {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        const auto next = [&]() -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + std::string(option));
            }
            return argv[++index];
        };
        if (option == "--updates") {
            options.updates = std::stoull(next());
            if (options.updates == 0) {
                throw std::runtime_error("--updates must be positive");
            }
        } else if (option == "--out") {
            options.out_dir = next();
        } else {
            throw std::runtime_error("unknown option " + std::string(option));
        }
    }
    return options;
}

/// Publish one synthetic depth snapshot with an oscillating best quote so the
/// open order is repriced on every update (cancel + new each cycle).
auto publishSnapshot(Exchange::MarketUpdateLFQueue& queue,
                                         std::uint64_t sequence) -> void {
    auto* slot = queue.getNextToWriteTo();
    slot->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    slot->ticker_id_ = 0;
    slot->published_ns_ = Common::nowSteadyNs();
    const auto bid_price = 100 + static_cast<Common::Price>(sequence % 3) * 2;
    slot->depth_snapshot_.last_price_ = bid_price + 1;
    slot->depth_snapshot_.bids_[0] = {bid_price, 2};
    slot->depth_snapshot_.asks_[0] = {bid_price + 1, 6};
    queue.updateWriteIndex();
}

/// Deterministic synthetic venue: accepts every new order and honors every
/// cancel without filling, so the position stays flat and the mock provider
/// keeps driving the full order cycle.
class SyntheticVenue final {
  public:
    auto handleRequest(std::size_t sequence,
                                             const Exchange::ClientRequest& request,
                                             Trading::OrderGateway& gateway) -> void {
        Exchange::ClientResponse response;
        response.type_ = request.type_ == Exchange::ClientRequestType::CANCEL
                                                 ? Exchange::ClientResponseType::CANCELED
                                                 : Exchange::ClientResponseType::ACCEPTED;
        response.client_id_ = request.client_id_;
        response.ticker_id_ = request.ticker_id_;
        response.client_order_id_ = request.order_id_;
        response.venue_order_id_ = sequence;
        response.side_ = request.side_;
        response.price_ = request.price_;
        response.exec_qty_ = 0;
        response.leaves_qty_ = request.qty_;
        response.order_type_ = request.order_type_;
        response.offset_ = request.offset_;
        gateway.publishClientResponse(sequence, response);
    }
};

struct SegmentStats {
    std::uint64_t count = 0;
    std::vector<std::uint64_t> latencies;
    std::uint64_t sum = 0;
};

/// Nearest-rank percentile over ascending-sorted values.
auto percentile(const std::vector<std::uint64_t>& sorted, double q) -> std::uint64_t {
    const auto rank =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(q * sorted.size())));
    return sorted[rank - 1];
}

auto parseUnsigned(std::string_view text, std::uint64_t& value) -> bool {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr != text.data();
}

/// Aggregate the TTT producer log so the printed summary is exactly the set
/// of records written during the run.
auto summarizeTttLog(const std::filesystem::path& ttt_log) -> std::map<std::string, SegmentStats> {
    std::map<std::string, SegmentStats> segments;
    std::ifstream stream(ttt_log);
    if (!stream) {
        throw std::runtime_error("cannot read TTT log " + ttt_log.string());
    }
    std::string line;
    while (std::getline(stream, line)) {
        const auto event_at = line.find("event=ttt_");
        if (event_at == std::string::npos) {
            continue;
        }
        const auto kind_start = event_at + std::strlen("event=");
        const auto kind_end = line.find(' ', kind_start);
        const auto kind = line.substr(kind_start,
                                                                    kind_end == std::string::npos
                                                                            ? std::string::npos
                                                                            : kind_end - kind_start);
        auto& stats = segments[kind];
        ++stats.count;
        const auto elapsed_at = line.find("elapsed_ns=");
        if (elapsed_at == std::string::npos) {
            continue;
        }
        auto elapsed = std::uint64_t{0};
        if (parseUnsigned(
                        std::string_view(line).substr(elapsed_at + std::strlen("elapsed_ns=")),
                        elapsed)) {
            stats.latencies.push_back(elapsed);
            stats.sum += elapsed;
        }
    }
    return segments;
}

auto printSummary(const std::map<std::string, SegmentStats>& segments) -> void {
    std::cout << "segment\tcount\tmean_ns\tp50_ns\tp90_ns\tp99_ns\tmax_ns\n";
    for (const auto& [kind, stats] : segments) {
        std::cout << kind << '\t' << stats.count << '\t';
        if (stats.latencies.empty()) {
            std::cout << "-\t-\t-\t-\t-";
        } else {
            auto sorted = stats.latencies;
            std::sort(sorted.begin(), sorted.end());
            const auto mean = static_cast<double>(stats.sum) / sorted.size();
            std::cout << std::fixed << std::setprecision(2) << mean << '\t' << percentile(sorted, 0.50)
                                << '\t' << percentile(sorted, 0.90) << '\t'
                                << percentile(sorted, 0.99) << '\t' << sorted.back();
        }
        std::cout << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        std::filesystem::create_directories(options.out_dir);

        Common::AsyncLogger logger(options.out_dir, kLoggerQueueCapacity,
                                                             std::chrono::milliseconds(1));
        Exchange::ClientRequestLFQueue requests(kQueueCapacity);
        Exchange::ClientResponseLFQueue responses(kQueueCapacity);
        Exchange::MarketUpdateLFQueue market_updates(kQueueCapacity);
        Trading::JevDecisionLFQueue decisions(kQueueCapacity);
        Trading::JevEvaluationStateLFQueue evaluations(kQueueCapacity);

        logger.start();
        Trading::TradeEngine engine(
                1, Trading::RiskLimits{10, 10, 0.0}, &requests, &responses,
                &market_updates, &decisions, &logger);
        engine.attachJevEvaluationQueue(&evaluations, std::chrono::milliseconds(0), 0);
        Trading::OrderGateway gateway(1, &requests, &responses, &logger);
        SyntheticVenue venue;
        gateway.setRequestHandler([&](std::size_t sequence,
                                                                    const Exchange::ClientRequest& request) {
            venue.handleRequest(sequence, request, gateway);
        });
        Trading::MockJevDecisionProvider provider;
        Trading::JevWorker worker(&evaluations, &decisions, &provider);

        const auto run_started = std::chrono::steady_clock::now();
        for (std::uint64_t sequence = 0; sequence < options.updates; ++sequence) {
            publishSnapshot(market_updates, sequence);
            // Synchronous pipeline drain: venue update -> evaluation ->
            // decision -> orders -> synthetic venue responses -> order updates.
            engine.processPending();
            worker.processPending();
            engine.processPending();
            gateway.processPending();
            engine.processPending();
        }
        const auto wall_ms = std::chrono::duration<double, std::milli>(
                                                             std::chrono::steady_clock::now() - run_started)
                                                     .count();
        logger.stop();

        const auto segments = summarizeTttLog(options.out_dir / "ttt-1.log");
        printSummary(segments);
        std::cout << std::fixed << std::setprecision(2) << "updates=" << options.updates
                            << " wall_ms=" << wall_ms
                            << " ttt_log=" << (options.out_dir / "ttt-1.log").string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jev_bench error: " << error.what() << '\n'
                            << "usage: jev_bench [--updates N] [--out DIR]\n";
        return 1;
    }
}
