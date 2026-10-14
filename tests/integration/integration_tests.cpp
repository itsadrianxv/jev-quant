#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/async_logger.h"
#include "common/lf_queue.h"
#include "common/time_utils.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/jev_capture.h"
#include "trading/strategy/jev_decision.h"
#include "trading/strategy/jev_http_client.h"
#include "trading/strategy/trade_engine.h"
#include "trading/strategy/jev_worker.h"
#include "trading/venue/venue_adapter.h"

#ifndef JEV_TEST_MOCK_SERVER_PATH
#define JEV_TEST_MOCK_SERVER_PATH "integration/mock_jev_server.py"
#endif

// Offline transport seam: exercise the same queue drains and REST rejection
// path as the adapter without opening a connection to Binance.
namespace Trading {
class BinanceVenueHarness {
  public:
    static void prepare(BinanceUmFuturesVenueAdapter& venue,
                        const Exchange::ClientRequest& live_order,
                        std::promise<std::thread::id>& processed,
                        std::promise<std::thread::id>& market_processed) {
        venue.depth_book_.installSnapshot(nlohmann::json{
                {"lastUpdateId", 10}, {"bids", nlohmann::json::array({nlohmann::json::array({"100", "2"})})},
                {"asks", nlohmann::json::array({nlohmann::json::array({"101", "3"})})}});
        venue.snapshot_pending_ = true;
        venue.live_orders_[live_order.order_id_] = live_order;
        venue.order_gateway_->setPollHandler([&venue] { venue.pollOrders(); });
        venue.market_data_->setPollHandler([&venue, &market_processed, reported=false]() mutable {
            venue.pollMarket();
            if (!reported) {
                reported=true;
                market_processed.set_value(std::this_thread::get_id());
            }
        });
        venue.order_gateway_->setRequestHandler([&venue, &processed](auto sequence, const auto& request) {
            venue.handleRequest(sequence, request);
            processed.set_value(std::this_thread::get_id());
        });
    }
    static void depth(BinanceUmFuturesVenueAdapter& venue) {
        *venue.depth_events_.getNextToWriteTo() = nlohmann::json{
                {"U", 11}, {"u", 11}, {"pu", 10},
                {"b", nlohmann::json::array({nlohmann::json::array({"100", "4"})})}, {"a", nlohmann::json::array()}};
        venue.depth_events_.updateWriteIndex();
    }
    static void fill(BinanceUmFuturesVenueAdapter& venue) {
        *venue.user_events_.getNextToWriteTo() = nlohmann::json{
                {"e", "ORDER_TRADE_UPDATE"},
                {"o", {{"c", "42"}, {"z", "1"}, {"X", "FILLED"}, {"i", 99}}}};
        venue.user_events_.updateWriteIndex();
    }
};
}  // namespace Trading

namespace {

constexpr auto kPollStep = std::chrono::milliseconds(2);
constexpr auto kWaitDeadline = std::chrono::seconds(5);
constexpr char kMockServerPath[] = JEV_TEST_MOCK_SERVER_PATH;
constexpr int kMockServerPort = 18765;

// Bounded wait helper: polls the predicate until it holds or the deadline
// passes. The predicate receives a string to record the last observed state,
// which the caller reports in its failure message.
struct WaitObservation {
    bool met = false;
    long waited_ms = 0;
    std::string observed;
};

template <typename Predicate>
auto waitFor(std::string_view what, std::chrono::milliseconds deadline,
             Predicate&& predicate) -> WaitObservation {
    WaitObservation observation;
    observation.observed = std::string(what) + " never observed";
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        if (predicate(observation.observed)) {
            observation.met = true;
            break;
        }
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        if (waited >= deadline) {
            break;
        }
        std::this_thread::sleep_for(kPollStep);
    }
    observation.waited_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
    return observation;
}

auto tempDir(const std::string& name) -> std::filesystem::path {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

auto readWholeFile(const std::filesystem::path& path) -> std::string {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

auto fileState(const std::filesystem::path& path) -> std::string {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error || !exists) {
        return "exists=false";
    }
    const auto size = std::filesystem::file_size(path, error);
    return "exists=true size=" + (error ? std::string("unknown") : std::to_string(size));
}

auto makeAttemptMeta(std::uint64_t evaluation_id) -> Trading::JevCaptureAttemptMeta {
    Trading::JevCaptureAttemptMeta meta;
    meta.evaluation_id_ = evaluation_id;
    meta.ticker_id_ = 0;
    meta.attempt_ = 0;
    meta.endpoint_ = "https://openrouter.ai/api/alpha/decisions";
    meta.model_ = "typesafe/jev-1.13";
    meta.timeout_ms_ = 4000;
    meta.max_retries_ = 0;
    meta.started_utc_ = Trading::JevHttpCapture::utcTimestampNow();
    return meta;
}

auto makeFlatEvaluationState(std::uint64_t evaluation_id) -> Trading::JevEvaluationState {
    Trading::JevEvaluationState state;
    state.evaluation_id_ = evaluation_id;
    state.ticker_id_ = 0;
    state.position_.net_position_ = 0;
    state.depth_snapshot_.last_price_ = 100;
    state.depth_snapshot_.bids_[0] = {99, 2};
    state.depth_snapshot_.asks_[0] = {101, 3};
    return state;
}

/// Owns one mock decision server child process for the duration of the test.
class MockDecisionServer {
  public:
    MockDecisionServer() {
        pid_ = ::fork();
        if (pid_ < 0) {
            start_error_ = "fork failed: " + std::string(std::strerror(errno));
            pid_ = -1;
            return;
        }
        if (pid_ == 0) {
            // Child: replace the image with the mock server; silence output.
            if (freopen("/dev/null", "w", stderr) == nullptr ||
                freopen("/dev/null", "w", stdout) == nullptr) {
                _exit(126);
            }
            ::execlp("python3", "python3", kMockServerPath,
                     static_cast<char*>(nullptr));
            _exit(127);
        }
    }

    ~MockDecisionServer() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, nullptr, 0);
        }
    }

    MockDecisionServer(const MockDecisionServer&) = delete;
    MockDecisionServer& operator=(const MockDecisionServer&) = delete;

    [[nodiscard]] auto waitForReady() const -> WaitObservation {
        if (!start_error_.empty()) {
            return {false, 0, start_error_};
        }
        return waitFor("mock server accepting connections", kWaitDeadline,
                       [](std::string& observed) {
                           const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
                           if (fd < 0) {
                               observed = "socket failed: " +
                                          std::string(std::strerror(errno));
                               return false;
                           }
                           sockaddr_in address{};
                           address.sin_family = AF_INET;
                           address.sin_port = htons(kMockServerPort);
                           address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                           const int connected =
                                   ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                                             sizeof(address));
                           const int connect_error = errno;
                           ::close(fd);
                           if (connected == 0) {
                               return true;
                           }
                           observed = "connect errno=" + std::to_string(connect_error) +
                                      " (" + std::string(std::strerror(connect_error)) + ")";
                           return false;
                       });
    }

  private:
    pid_t pid_ = -1;
    std::string start_error_;
};

}  // namespace

TEST(AsyncLoggerTest, WritesFixedBufferFormattedRecordsAndTruncatesOversizedOnes) {
    const auto output_dir = tempDir("jev_quant_async_logger_fixed_buffer");

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    auto engine_log = logger.registerProducer("TradeEngine", "trade-engine.log");
    logger.start();

    engine_log.logFixed(Common::LogLevel::INFO, [](auto& out) {
        out.append("event=client_response_consumed client_id=").append(7)
                .append(" ticker_id=").append(0);
    });
    const std::string oversized(Common::AsyncLogger::kMessageCapacity + 32, 'x');
    engine_log.logFixed(Common::LogLevel::WARN, [&](auto& out) {
        out.append("fixed_prefix ").append(oversized);
    });
    logger.stop();

    const std::string engine_text = readWholeFile(output_dir / "trade-engine.log");
    EXPECT_NE(std::string::npos,
              engine_text.find("event=client_response_consumed client_id=7 ticker_id=0"))
            << "observed file: " << engine_text;
    EXPECT_NE(std::string::npos, engine_text.find("fixed_prefix "))
            << "observed file: " << engine_text;
    // The fixed-buffer path truncates at the same record capacity.
    EXPECT_LT(engine_text.size(), Common::AsyncLogger::kMessageCapacity + 256)
            << "observed size: " << engine_text.size();
    EXPECT_EQ(std::string::npos,
              engine_text.find(std::string(Common::AsyncLogger::kMessageCapacity, 'x')));
}

TEST(AsyncLoggerTest, RoutesEachProducerToItsOwnFile) {
    const auto output_dir = tempDir("jev_quant_async_logger_contract");

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    auto engine_log = logger.registerProducer("TradeEngine", "trade-engine.log");
    auto gateway_log = logger.registerProducer("OrderGateway", "order-gateway.log");
    logger.start();

    engine_log.log(Common::LogLevel::INFO, "market update");
    gateway_log.log(Common::LogLevel::ERROR, "order rejected");
    engine_log.log(Common::LogLevel::WARN,
                   std::string(Common::AsyncLogger::kMessageCapacity + 32, 'x'));
    logger.stop();

    const std::string engine_text = readWholeFile(output_dir / "trade-engine.log");
    const std::string gateway_text = readWholeFile(output_dir / "order-gateway.log");
    EXPECT_NE(std::string::npos, engine_text.find("[INFO] [TradeEngine] market update"))
            << "observed file: " << engine_text;
    EXPECT_NE(std::string::npos, engine_text.find("[WARN] [TradeEngine]"))
            << "observed file: " << engine_text;
    EXPECT_LT(engine_text.size(), Common::AsyncLogger::kMessageCapacity + 256)
            << "observed size: " << engine_text.size();
    EXPECT_NE(std::string::npos,
              gateway_text.find("[ERROR] [OrderGateway] order rejected"))
            << "observed file: " << gateway_text;
}

TEST(AsyncLoggerTest, DropsRecordsWhenTheQueueOverflows) {
    const auto output_dir = tempDir("jev_quant_async_logger_overflow_contract");

    Common::AsyncLogger logger(output_dir, 1, std::chrono::milliseconds(1));
    auto producer = logger.registerProducer("Overflow", "overflow.log");

    // Fill the queue before starting the consumer so overflow is deterministic.
    producer.log(Common::LogLevel::DEBUG, "first");
    producer.log(Common::LogLevel::DEBUG, "dropped");
    logger.start();
    logger.stop();

    const std::string text = readWholeFile(output_dir / "overflow.log");
    EXPECT_NE(std::string::npos, text.find(" first\n"))
            << "observed file: " << text;
    EXPECT_EQ(std::string::npos, text.find("dropped"))
            << "observed file: " << text;
}

TEST(AsyncLoggerTest, MakesLateRegisteredProducersVisibleAfterStart) {
    const auto output_dir = tempDir("jev_quant_async_logger_startup_contract");

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    logger.start();
    auto venue = logger.registerProducer("Venue", "venue.log");
    venue.log(Common::LogLevel::INFO, "event=startup_stage stage=exchange_info");

    const auto venue_path = output_dir / "venue.log";
    const auto wait = waitFor("venue log to receive the startup record",
                              kWaitDeadline, [&venue_path](std::string& observed) {
                                  observed = fileState(venue_path);
                                  std::error_code error;
                                  return std::filesystem::exists(venue_path, error) && !error &&
                                         std::filesystem::file_size(venue_path, error) > 0 &&
                                         !error;
                              });
    ASSERT_TRUE(wait.met) << "timed out after " << wait.waited_ms << "ms; observed "
                          << wait.observed;

    auto gateway = logger.registerProducer("OrderGateway", "order-gateway.log");
    gateway.log(Common::LogLevel::INFO, "event=component_started");
    logger.stop();

    const std::string gateway_text = readWholeFile(output_dir / "order-gateway.log");
    EXPECT_NE(std::string::npos, gateway_text.find("event=component_started"))
            << "observed file: " << gateway_text;
}

TEST(AsyncLoggerTest, WritesFromAWorkerThreadAfterBinding) {
    const auto output_dir = tempDir("jev_quant_async_logger_binding_contract");

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    auto producer = logger.registerProducer("Worker", "worker.log");
    std::jthread worker([producer = std::move(producer)]() mutable {
        producer.bindToCurrentThread();
        producer.log(Common::LogLevel::INFO, "event=worker_bound");
    });
    worker.join();
    // Bounded wait for the worker-thread binding to become visible.
    logger.waitForProducerBindings(1, kWaitDeadline);
    logger.start();
    logger.stop();

    const std::string text = readWholeFile(output_dir / "worker.log");
    EXPECT_NE(std::string::npos, text.find("event=worker_bound"))
            << "observed file: " << text;
}

TEST(LFQueueSpscTest, ConveysTenThousandMessagesBetweenThreadsInOrder) {
    constexpr int kMessageCount = 10'000;
    constexpr auto kStressDeadline = std::chrono::seconds(10);

    Common::LFQueue<int> queue(64);
    std::atomic<bool> producer_done{false};
    std::atomic<int> consumed{0};
    std::atomic<bool> in_order{true};
    std::atomic<int> first_mismatch{-1};
    std::atomic<bool> timed_out{false};
    std::atomic<long> timeout_waited_ms{0};

    std::jthread producer([&] {
        const auto started = std::chrono::steady_clock::now();
        for (int value = 0; value < kMessageCount; ++value) {
            int* slot = nullptr;
            while ((slot = queue.tryGetNextToWriteTo()) == nullptr) {
                const auto waited =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started);
                if (waited >= kStressDeadline) {
                    timed_out.store(true, std::memory_order_relaxed);
                    timeout_waited_ms.store(waited.count(), std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
            }
            *slot = value;
            queue.updateWriteIndex();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::jthread consumer([&] {
        const auto started = std::chrono::steady_clock::now();
        int expected = 0;
        while (!producer_done.load(std::memory_order_acquire) || queue.size() != 0) {
            const auto waited =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started);
            if (waited >= kStressDeadline) {
                timed_out.store(true, std::memory_order_relaxed);
                timeout_waited_ms.store(waited.count(), std::memory_order_relaxed);
                return;
            }
            const int* slot = queue.getNextToRead();
            if (slot == nullptr) {
                std::this_thread::yield();
                continue;
            }
            if (*slot != expected && in_order.load(std::memory_order_relaxed)) {
                in_order.store(false, std::memory_order_relaxed);
                first_mismatch.store(expected, std::memory_order_relaxed);
                return;
            }
            ++expected;
            consumed.fetch_add(1, std::memory_order_relaxed);
            queue.updateReadIndex();
        }
        if (expected != kMessageCount && in_order.load(std::memory_order_relaxed)) {
            in_order.store(false, std::memory_order_relaxed);
            first_mismatch.store(expected, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(timed_out.load(std::memory_order_relaxed))
            << "the queue stalled; waited " << timeout_waited_ms.load() << "ms";
    EXPECT_TRUE(in_order.load(std::memory_order_relaxed))
            << "first out-of-order expected value " << first_mismatch.load()
            << " after " << consumed.load() << " consumed messages";
    EXPECT_EQ(kMessageCount, consumed.load(std::memory_order_relaxed))
            << "observed consumed count";
}

TEST(JevHttpCaptureTest, StaysDisabledByDefault) {
    Trading::JevHttpCapture capture;
    EXPECT_FALSE(capture.enabled());

    Trading::JevCaptureAttemptMeta meta;
    const auto context = capture.beginAttempt(meta, "{}");
    EXPECT_FALSE(context.has_value());
    capture.completeAttempt(context, meta, "{}", "{}", 0, "OK", 200, 0.0, "ok", "");
}

TEST(JevHttpCaptureTest, RecordsAttemptFilesAndMetadataForEnabledCaptures) {
    const auto output_dir = tempDir("jev_quant_capture_files_contract");
    Trading::JevHttpCapture capture(Trading::JevCaptureConfig{output_dir.string(), "run-A"});
    ASSERT_TRUE(capture.enabled());
    EXPECT_EQ("run-A", capture.runId());

    auto meta = makeAttemptMeta(12);
    const auto context = capture.beginAttempt(meta, "abc");
    ASSERT_TRUE(context.has_value());
    capture.completeAttempt(context, meta, "abc", "response-bytes", 0, "OK", 200, 5.5,
                            "ok", "");

    EXPECT_EQ("abc", readWholeFile(context->directory_ / "request.json"));
    // SHA-256 test vector for the literal string "abc".
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              Trading::JevHttpCapture::sha256Hex("abc"));

    std::ifstream metadata_file(context->directory_ / "metadata.json");
    nlohmann::json metadata;
    metadata_file >> metadata;
    EXPECT_EQ("run-A", metadata.at("run_id").get<std::string>());
    EXPECT_EQ(12, metadata.at("evaluation_id"));
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              metadata.at("request_sha256").get<std::string>());
    EXPECT_EQ(Trading::JevHttpCapture::sha256Hex("response-bytes"),
              metadata.at("response_sha256").get<std::string>());
    EXPECT_EQ(200, metadata.at("http_status"));
    EXPECT_EQ("ok", metadata.at("parse_outcome").get<std::string>());
    EXPECT_DOUBLE_EQ(5.5, metadata.at("elapsed_ms"));
    EXPECT_TRUE(metadata.contains("source_revision"));
    EXPECT_TRUE(metadata.contains("binary_sha256"));
    EXPECT_TRUE(metadata.contains("started_utc"));
    EXPECT_EQ("live", metadata.at("role").get<std::string>());
    EXPECT_EQ("", metadata.at("variant").get<std::string>());
    EXPECT_EQ("response-bytes", readWholeFile(context->directory_ / "response.raw"));
}

TEST(JevHttpCaptureTest, RecordsFailedAttemptsWithoutLeakingCredentialMaterial) {
    const auto output_dir = tempDir("jev_quant_capture_failure_contract");
    Trading::JevHttpConfig config;
    config.endpoint_ = "http://127.0.0.1:9/";
    config.api_key_ = "test-capture-only-key";
    config.timeout_ms_ = 300;
    config.capture_ = Trading::JevCaptureConfig{output_dir.string(), "run-B"};
    Trading::JevHttpClient client(config);
    const auto state = makeFlatEvaluationState(7);
    const auto expected_body = client.buildRequestBody(state);

    EXPECT_THROW((void)client.evaluate(state), std::runtime_error);

    const auto run_dir = output_dir / "run-B";
    ASSERT_TRUE(std::filesystem::exists(run_dir))
            << "observed output dir: " << fileState(output_dir);

    bool found_attempt = false;
    for (const auto& entry : std::filesystem::directory_iterator(run_dir)) {
        found_attempt = true;
        const std::string request_bytes =
                readWholeFile(entry.path() / "request.json");
        EXPECT_EQ(expected_body, request_bytes) << "observed attempt dir: "
                                                << entry.path();
        std::ifstream metadata_stream(entry.path() / "metadata.json");
        nlohmann::json metadata;
        metadata_stream >> metadata;
        EXPECT_EQ(7, metadata.at("evaluation_id")) << "observed metadata: " << metadata;
        EXPECT_NE("OK", metadata.at("curl_outcome").get<std::string>())
                << "observed metadata: " << metadata;
        EXPECT_TRUE(metadata.at("parse_outcome").is_null())
                << "observed metadata: " << metadata;
        // No credential material may appear in any captured file: the
        // Authorization header is never part of the recorded bytes.
        const auto combined = request_bytes +
                              readWholeFile(entry.path() / "response.raw") +
                              readWholeFile(entry.path() / "metadata.json");
        EXPECT_EQ(std::string::npos, combined.find("Authorization"))
                << "observed attempt dir: " << entry.path();
    }
    EXPECT_TRUE(found_attempt) << "no attempt directory was written";
}

TEST(JevHttpDecisionPathTest, EvaluatesDecisionsAgainstTheLocalMockServer) {
    const MockDecisionServer server;
    const auto ready = server.waitForReady();
    ASSERT_TRUE(ready.met) << "mock server not reachable within "
                           << ready.waited_ms << "ms; observed " << ready.observed;

    Trading::JevHttpConfig config;
    config.endpoint_ = "http://127.0.0.1:" + std::to_string(kMockServerPort) + "/";
    config.api_key_ = "mock-server-test-key";
    config.timeout_ms_ = 4000;
    config.max_retries_ = 0;
    Trading::JevHttpClient client(config);

    const auto state = makeFlatEvaluationState(42);
    const auto decision = client.evaluate(state);

    EXPECT_EQ(42u, decision.evaluation_id_);
    EXPECT_EQ(Trading::JevBias::LONG, decision.bias_);
    EXPECT_EQ(Trading::JevIntent::HOLD, decision.intent_);
    EXPECT_DOUBLE_EQ(0.9, decision.long_probability_);
    EXPECT_DOUBLE_EQ(0.1, decision.short_probability_);
    EXPECT_DOUBLE_EQ(0.1, decision.open_probability_);
    EXPECT_DOUBLE_EQ(0.0, decision.close_probability_);
    EXPECT_DOUBLE_EQ(0.8, decision.hold_probability_);
    EXPECT_DOUBLE_EQ(0.7, decision.confidence_);
}

TEST(TttInstrumentationTest, RecordsLatencyEventsForStampedMessages) {
    const auto output_dir = tempDir("jev_quant_ttt_instrumentation");

    Common::AsyncLogger logger(output_dir, 64, std::chrono::milliseconds(1));
    Exchange::ClientRequestLFQueue requests(32);
    Exchange::ClientResponseLFQueue responses(32);
    Exchange::MarketUpdateLFQueue market_updates(32);
    Trading::JevDecisionLFQueue decisions(32);
    Trading::TradeEngine engine(7, Trading::RiskLimits{10, 10, 0.0}, &requests,
                                &responses, &market_updates, &decisions, std::nullopt,
                                logger.registerProducer("TTT", "ttt-7.log"));
    logger.start();

    const std::uint64_t past = Common::nowSteadyNs() - 5'000'000ULL;

    auto* update = market_updates.getNextToWriteTo();
    update->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update->ticker_id_ = 0;
    update->published_ns_ = past;
    update->depth_snapshot_.bids_[0] = {100, 2};
    update->depth_snapshot_.asks_[0] = {101, 3};
    market_updates.updateWriteIndex();

    auto* decision = decisions.getNextToWriteTo();
    decision->evaluation_id_ = 1;
    decision->ticker_id_ = 0;
    decision->bias_ = Trading::JevBias::LONG;
    decision->intent_ = Trading::JevIntent::OPEN;
    decision->enqueued_ns_ = past;
    decisions.updateWriteIndex();
    engine.registerEvaluation(0, 1);

    ASSERT_TRUE(engine.processPending());

    auto* request = requests.getNextToWriteTo();
    ASSERT_NE(nullptr, request);
    request->enqueued_ns_ = past;
    requests.updateWriteIndex();

    auto* response = responses.getNextToWriteTo();
    response->type_ = Exchange::ClientResponseType::ACCEPTED;
    response->client_id_ = 7;
    response->ticker_id_ = 0;
    response->client_order_id_ = request->order_id_;
    response->published_ns_ = past;
    responses.updateWriteIndex();

    ASSERT_TRUE(engine.processPending());
    logger.stop();

    const std::string ttt_text = readWholeFile(output_dir / "ttt-7.log");
    EXPECT_NE(std::string::npos, ttt_text.find("event=ttt_market_data_consumed"))
            << "observed file: " << ttt_text;
    EXPECT_NE(std::string::npos, ttt_text.find("event=ttt_decision_consumed"))
            << "observed file: " << ttt_text;
    EXPECT_NE(std::string::npos, ttt_text.find("event=ttt_decision_to_order"))
            << "observed file: " << ttt_text;
    EXPECT_NE(std::string::npos, ttt_text.find("event=ttt_order_round_trip"))
            << "observed file: " << ttt_text;
    EXPECT_NE(std::string::npos, ttt_text.find("elapsed_ns="))
            << "observed file: " << ttt_text;
}

TEST(TttInstrumentationTest, RecordsNoLatencyEventsForUnstampedMessages) {
    const auto output_dir = tempDir("jev_quant_ttt_unstamped");

    Common::AsyncLogger logger(output_dir, 64, std::chrono::milliseconds(1));
    Exchange::ClientRequestLFQueue requests(32);
    Exchange::ClientResponseLFQueue responses(32);
    Exchange::MarketUpdateLFQueue market_updates(32);
    Trading::JevDecisionLFQueue decisions(32);
    Trading::TradeEngine engine(7, Trading::RiskLimits{10, 10, 0.0}, &requests,
                                &responses, &market_updates, &decisions, std::nullopt,
                                logger.registerProducer("TTT", "ttt-7.log"));
    logger.start();

    auto* decision = decisions.getNextToWriteTo();
    decision->evaluation_id_ = 1;
    decision->ticker_id_ = 0;
    decision->bias_ = Trading::JevBias::LONG;
    decision->intent_ = Trading::JevIntent::OPEN;
    decisions.updateWriteIndex();
    engine.registerEvaluation(0, 1);
    ASSERT_TRUE(engine.processPending());

    logger.stop();

    // Unstamped messages must never produce latency records (drop records
    // carry no elapsed_ns and are the only ttt_ lines expected here).
    const std::string ttt_text = readWholeFile(output_dir / "ttt-7.log");
    EXPECT_EQ(std::string::npos, ttt_text.find("elapsed_ns="))
            << "observed file: " << ttt_text;
}

TEST(ComponentLoggingTest, InjectedHandlesWriteLifecycleRecordsFromWorkers) {
    const auto output_dir = tempDir("jev_quant_component_handles");
    Common::AsyncLogger logger(output_dir, 64, std::chrono::milliseconds(1));
    Exchange::ClientRequestLFQueue requests(8);
    Exchange::ClientResponseLFQueue responses(8);
    Exchange::MarketUpdateLFQueue updates(8);
    Trading::JevDecisionLFQueue decisions(8);
    Trading::JevEvaluationStateLFQueue evaluations(8);
    Trading::MockJevDecisionProvider provider;
    auto engine_log = logger.registerProducer("TradeEngine", "trade-engine-7.log");
    auto gateway_log = logger.registerProducer("OrderGateway", "order-gateway-7.log");
    auto worker_log = logger.registerProducer("JevWorker", "jev-worker.log");
    Trading::TradeEngine engine(7, Trading::RiskLimits{10, 10, 0.0}, &requests,
            &responses, &updates, &decisions, std::move(engine_log));
    Trading::OrderGateway gateway(7, &requests, &responses, std::move(gateway_log));
    Trading::JevWorker worker(&evaluations, &decisions, &provider, std::move(worker_log));
    logger.start();
    engine.start();
    gateway.start();
    worker.start();
    // run() writes both lifecycle records even if stop precedes its loop.
    // Joining each producer, then draining the logger, needs no timing sleeps.
    engine.stop();
    gateway.stop();
    worker.stop();
    logger.stop();
    for (const auto* file : {"trade-engine-7.log", "order-gateway-7.log", "jev-worker.log"}) {
        const auto text = readWholeFile(output_dir / file);
        EXPECT_NE(std::string::npos, text.find("event=component_started")) << file << text;
        EXPECT_NE(std::string::npos, text.find("event=component_stopped")) << file << text;
        EXPECT_EQ(std::string::npos, text.find("[TTT]")) << file << text;
    }
    EXPECT_FALSE(std::filesystem::exists(output_dir / "ttt-7.log"));
}

TEST(ComponentLoggingTest, MarketStartupDoesNotBindTheCompositionThread) {
    const auto output_dir = tempDir("jev_quant_market_handle");
    Common::AsyncLogger logger(output_dir, 16, std::chrono::milliseconds(1));
    Exchange::MarketUpdateLFQueue updates(8);
    auto handle = logger.registerProducer("MarketData", "market-data-7.log");
    Trading::MarketDataConsumer market(7, &updates, std::move(handle), true);
    std::promise<std::thread::id> published;
    auto done = published.get_future();
    market.setPollHandler([&, once=false]() mutable {
        if (once) return;
        once=true;
        Exchange::MarketUpdate update;
        update.ticker_id_ = 0;
        market.publishSequencedMarketUpdate(1, update);
        EXPECT_THROW(market.publishSequencedMarketUpdate(3, update), std::logic_error);
        published.set_value(std::this_thread::get_id());
    });
    logger.start();
    market.start();
    const auto status = done.wait_for(kWaitDeadline);
    market.stop();
    logger.stop();
    ASSERT_EQ(std::future_status::ready, status);
    EXPECT_NE(std::this_thread::get_id(), done.get());
    const auto text = readWholeFile(output_dir / "market-data-7.log");
    EXPECT_NE(std::string::npos, text.find("event=component_stopped")) << text;
    EXPECT_NE(std::string::npos, text.find("[MarketData] event=component_started client_id=7")) << text;
    EXPECT_NE(std::string::npos, text.find("event=market_update_published")) << text;
    EXPECT_NE(std::string::npos, text.find("event=market_sequence_gap")) << text;
    EXPECT_EQ(1U, updates.size());
}

TEST(ComponentLoggingTest, BinanceConstructorLeavesHandleUnbound) {
    const auto output_dir = tempDir("jev_quant_venue_handle");
    Common::AsyncLogger logger(output_dir, 16, std::chrono::milliseconds(1));
    auto handle = logger.registerProducer("Venue", "venue-0.log");
    Trading::BinanceUmFuturesVenueAdapter venue(nullptr, nullptr, {}, std::move(handle));
    logger.start();
    std::jthread lifecycle([&] {
        // Dependency validation fails before any network request.
        EXPECT_THROW(venue.prepare(), std::runtime_error);
        venue.stop();
    });
    lifecycle.join();
    logger.stop();
    const auto text = readWholeFile(output_dir / "venue-0.log");
    EXPECT_NE(std::string::npos, text.find("[Venue] event=component_started")) << text;
    EXPECT_NE(std::string::npos, text.find("[Venue] event=component_stopped")) << text;
}

TEST(ComponentLoggingTest, WorkerBindsOrdinaryAndTttChannelsIndependently) {
    const auto output_dir = tempDir("jev_quant_engine_handles");
    Common::AsyncLogger logger(output_dir, 64, std::chrono::milliseconds(1));
    Exchange::ClientRequestLFQueue requests(8);
    Exchange::ClientResponseLFQueue responses(8);
    Exchange::MarketUpdateLFQueue updates(8);
    Trading::JevDecisionLFQueue decisions(8);
    Trading::TradeEngine engine(7, Trading::RiskLimits{10, 10, 0.0}, &requests,
            &responses, &updates, &decisions,
            logger.registerProducer("TradeEngine", "trade-engine-7.log"),
            logger.registerProducer("TTT", "ttt-7.log"));
    std::promise<void> consumed;
    auto done = consumed.get_future();
    engine.onMarketUpdate = [&](const auto&, const auto&) { consumed.set_value(); };
    auto* update = updates.getNextToWriteTo();
    update->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update->ticker_id_ = 0;
    update->published_ns_ = Common::nowSteadyNs() - 1;
    update->depth_snapshot_.bids_[0] = {100, 2};
    update->depth_snapshot_.asks_[0] = {101, 3};
    updates.updateWriteIndex();
    logger.start();
    engine.start();
    const auto status = done.wait_for(kWaitDeadline);
    engine.stop();
    logger.stop();
    ASSERT_EQ(std::future_status::ready, status);
    const auto ordinary = readWholeFile(output_dir / "trade-engine-7.log");
    const auto ttt = readWholeFile(output_dir / "ttt-7.log");
    EXPECT_NE(std::string::npos, ordinary.find("[TradeEngine] event=component_started"));
    EXPECT_EQ(std::string::npos, ordinary.find("event=ttt_"));
    EXPECT_NE(std::string::npos, ttt.find("[TTT] event=ttt_market_data_consumed"));
    EXPECT_EQ(std::string::npos, ttt.find("event=component_started"));
}

TEST(ComponentLoggingTest, ExplicitlyDisabledHandlesStillPublishMessages) {
    Exchange::ClientRequestLFQueue requests(8);
    Exchange::ClientResponseLFQueue responses(8);
    Exchange::MarketUpdateLFQueue updates(8);
    Trading::JevDecisionLFQueue decisions(8);
    Trading::JevEvaluationStateLFQueue evaluations(8);
    Trading::MockJevDecisionProvider provider;
    Trading::TradeEngine engine(7, Trading::RiskLimits{10, 10, 0.0}, &requests,
            &responses, &updates, &decisions, std::nullopt, std::nullopt);
    Trading::MarketDataConsumer market(7, &updates, std::nullopt, true);
    Trading::OrderGateway gateway(7, &requests, &responses, std::nullopt);
    Trading::JevWorker worker(&evaluations, &decisions, &provider, std::nullopt);
    Exchange::MarketUpdate update;
    update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update.ticker_id_ = 0;
    update.depth_snapshot_.bids_[0] = {100, 2};
    update.depth_snapshot_.asks_[0] = {101, 3};
    market.publishMarketUpdate(update);
    EXPECT_TRUE(engine.processPending());
    EXPECT_FALSE(gateway.processPending());
    EXPECT_FALSE(worker.processPending());
}

TEST(ComponentLoggingTest, BinanceProcessesMarketDataIndependentlyOfTheGateway) {
    const auto output_dir = tempDir("jev_quant_binance_queue_handles");
    Common::AsyncLogger logger(output_dir, 64, std::chrono::milliseconds(1));
    Exchange::ClientRequestLFQueue requests(8);
    Exchange::ClientResponseLFQueue responses(8);
    Exchange::MarketUpdateLFQueue updates(8);
    Trading::MarketDataConsumer market(7, &updates,
            logger.registerProducer("MarketData", "market-data-7.log"), true);
    Trading::OrderGateway gateway(7, &requests, &responses,
            logger.registerProducer("OrderGateway", "order-gateway-7.log"));
    Trading::BinanceUmFuturesConfig config;
    config.ticker_id = 0;
    Trading::BinanceUmFuturesVenueAdapter venue(&market, &gateway, std::move(config));
    Exchange::ClientRequest live_order;
    live_order.client_id_ = 7;
    live_order.order_id_ = 42;
    live_order.ticker_id_ = 0;
    live_order.qty_ = 1;
    std::promise<std::thread::id> processed, market_processed;
    auto done = processed.get_future();
    auto market_done = market_processed.get_future();
    Trading::BinanceVenueHarness::prepare(venue, live_order, processed, market_processed);
    std::jthread depth([&] { Trading::BinanceVenueHarness::depth(venue); });
    std::jthread user([&] { Trading::BinanceVenueHarness::fill(venue); });
    depth.join();
    user.join();
    auto* request = requests.getNextToWriteTo();
    *request = live_order;
    request->type_ = Exchange::ClientRequestType::NEW;
    request->order_id_ = 43;
    request->ticker_id_ = 1;  // Rejected by mapOrder before any REST call.
    requests.updateWriteIndex();
    logger.start();
    market.start();
    // Market processing must finish even while the gateway has not started.
    const auto market_status = market_done.wait_for(kWaitDeadline);
    EXPECT_EQ(std::future_status::ready, market_status);
    EXPECT_EQ(2U, updates.size());
    gateway.start();
    const auto status = done.wait_for(kWaitDeadline);
    market.stop();
    venue.quiesce();
    gateway.stop();
    venue.stop();
    logger.stop();
    ASSERT_EQ(std::future_status::ready, status);
    ASSERT_EQ(std::future_status::ready, market_status);
    const auto market_thread = market_done.get();
    EXPECT_NE(std::this_thread::get_id(), market_thread);
    EXPECT_NE(done.get(), market_thread);
    ASSERT_EQ(2U, updates.size());
    const auto first_quantity = updates.getNextToRead()->depth_snapshot_.bids_[0].qty_;
    updates.updateReadIndex();
    EXPECT_GT(updates.getNextToRead()->depth_snapshot_.bids_[0].qty_, first_quantity);
    ASSERT_EQ(2U, responses.size());
    EXPECT_EQ(Exchange::ClientResponseType::FILLED, responses.getNextToRead()->type_);
    responses.updateReadIndex();
    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, responses.getNextToRead()->type_);
    const auto market_text = readWholeFile(output_dir / "market-data-7.log");
    const auto gateway_text = readWholeFile(output_dir / "order-gateway-7.log");
    EXPECT_NE(std::string::npos, market_text.find("[MarketData] event=market_update_published"));
    EXPECT_NE(std::string::npos, gateway_text.find("event=client_response_published client_id=7 sequence=1"));
    EXPECT_NE(std::string::npos, gateway_text.find("event=client_response_published client_id=7 sequence=2"));
    EXPECT_EQ(std::string::npos, gateway_text.find("event=response_sequence_gap"));
}
