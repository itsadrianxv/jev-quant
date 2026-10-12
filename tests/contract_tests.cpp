#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "common/async_logger.h"
#include "common/lf_queue.h"
#include "common/time_utils.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/jev_capture.h"
#include "trading/strategy/jev_decision.h"
#include "trading/strategy/jev_http_client.h"
#include "trading/strategy/jev_worker.h"
#include "trading/strategy/market_order_book.h"
#include "trading/strategy/om_order.h"
#include "trading/strategy/trade_engine.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"
#include "simulated_venue.h"
namespace {

[[noreturn]] void checkFailure(const char* condition, const char* file,
                                                                int line) {
    std::cerr << "check failed: " << condition << " (" << file << ":" << line
                        << ")\n";
    std::abort();
}

#define CHECK(condition)                                                    \
    ((condition) ? static_cast<void>(0)                                       \
                               : checkFailure(#condition, __FILE__, __LINE__))

void testAsyncLoggerFixedBufferContract() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                            "jev_quant_async_logger_fixed_buffer";
    std::filesystem::remove_all(output_dir);

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    auto engine_log = logger.registerProducer("TradeEngine", "trade-engine.log");
    logger.start();

    engine_log.logFixed(Common::LogLevel::INFO, [](Common::LogWriter& out) {
        out.append("event=client_response_consumed client_id=").append(7)
                .append(" ticker_id=").append(0);
    });
    const std::string oversized(Common::AsyncLogger::kMessageCapacity + 32, 'x');
    engine_log.logFixed(Common::LogLevel::WARN, [&](Common::LogWriter& out) {
        out.append("fixed_prefix ").append(oversized);
    });
    logger.stop();

    std::ifstream engine_file(output_dir / "trade-engine.log");
    const std::string engine_text((std::istreambuf_iterator<char>(engine_file)),
                                  std::istreambuf_iterator<char>());
    CHECK(engine_text.find("event=client_response_consumed client_id=7 ticker_id=0") !=
          std::string::npos);
    CHECK(engine_text.find("fixed_prefix ") != std::string::npos);
    // The fixed-buffer path truncates at the same record capacity.
    CHECK(engine_text.size() < Common::AsyncLogger::kMessageCapacity + 256);
    CHECK(engine_text.find(std::string(Common::AsyncLogger::kMessageCapacity, 'x')) ==
          std::string::npos);
}

void testAsyncLoggerContract() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_async_logger_contract";
    std::filesystem::remove_all(output_dir);

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    auto engine_log = logger.registerProducer("TradeEngine", "trade-engine.log");
    auto gateway_log = logger.registerProducer("OrderGateway", "order-gateway.log");
    logger.start();

    engine_log.log(Common::LogLevel::INFO, "market update");
    gateway_log.log(Common::LogLevel::ERROR, "order rejected");
    engine_log.log(Common::LogLevel::WARN,
                                  std::string(Common::AsyncLogger::kMessageCapacity + 32, 'x'));
    logger.stop();

    std::ifstream engine_file(output_dir / "trade-engine.log");
    std::ifstream gateway_file(output_dir / "order-gateway.log");
    const std::string engine_text((std::istreambuf_iterator<char>(engine_file)),
                                                                std::istreambuf_iterator<char>());
    const std::string gateway_text((std::istreambuf_iterator<char>(gateway_file)),
                                                                  std::istreambuf_iterator<char>());
    CHECK(engine_text.find("[INFO] [TradeEngine] market update") !=
                std::string::npos);
    CHECK(engine_text.find("[WARN] [TradeEngine]") != std::string::npos);
    CHECK(engine_text.size() < Common::AsyncLogger::kMessageCapacity + 256);
    CHECK(gateway_text.find("[ERROR] [OrderGateway] order rejected") !=
                std::string::npos);

    std::filesystem::remove_all(output_dir);
}

void testAsyncLoggerQueueOverflow() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_async_logger_overflow_contract";
    std::filesystem::remove_all(output_dir);

    Common::AsyncLogger logger(output_dir, 1, std::chrono::milliseconds(1));
    auto producer = logger.registerProducer("Overflow", "overflow.log");
    logger.start();

    producer.log(Common::LogLevel::DEBUG, "first");
    producer.log(Common::LogLevel::DEBUG, "dropped");
    logger.stop();

    std::ifstream file(output_dir / "overflow.log");
    const std::string text((std::istreambuf_iterator<char>(file)),
                                                  std::istreambuf_iterator<char>());
    CHECK(text.find(" first\n") != std::string::npos);
    CHECK(text.find("dropped") == std::string::npos);
    std::filesystem::remove_all(output_dir);
}

void testAsyncLoggerLateRegistrationAndStartupVisibility() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_async_logger_startup_contract";
    std::filesystem::remove_all(output_dir);

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    logger.start();
    auto venue = logger.registerProducer("Venue", "venue.log");
    venue.log(Common::LogLevel::INFO, "event=startup_stage stage=exchange_info");

    const auto venue_path = output_dir / "venue.log";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::filesystem::file_size(venue_path) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(std::filesystem::file_size(venue_path) > 0);

    auto gateway = logger.registerProducer("OrderGateway", "order-gateway.log");
    gateway.log(Common::LogLevel::INFO, "event=component_started");
    logger.stop();

    std::ifstream gateway_file(output_dir / "order-gateway.log");
    const std::string gateway_text((std::istreambuf_iterator<char>(gateway_file)),
                                   std::istreambuf_iterator<char>());
    CHECK(gateway_text.find("event=component_started") != std::string::npos);
    std::filesystem::remove_all(output_dir);
}

void testAsyncLoggerWorkerBinding() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_async_logger_binding_contract";
    std::filesystem::remove_all(output_dir);

    Common::AsyncLogger logger(output_dir, 8, std::chrono::milliseconds(1));
    auto producer = logger.registerProducer("Worker", "worker.log");
    std::jthread worker([producer = std::move(producer)]() mutable {
        producer.bindToCurrentThread();
        producer.log(Common::LogLevel::INFO, "event=worker_bound");
    });
    worker.join();
    logger.waitForProducerBindings(1, std::chrono::seconds(5));
    logger.start();
    logger.stop();

    std::ifstream file(output_dir / "worker.log");
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    CHECK(text.find("event=worker_bound") != std::string::npos);
    std::filesystem::remove_all(output_dir);
}

void testQueueSpscConcurrency() {
    constexpr int message_count = 10'000;
    Common::LFQueue<int> queue(64);
    std::atomic<bool> producer_done{false};
    std::atomic<int> consumed{0};

    std::jthread producer([&] {
        for (int value = 0; value < message_count; ++value) {
            int* slot = nullptr;
            while ((slot = queue.tryGetNextToWriteTo()) == nullptr) {
                std::this_thread::yield();
            }
            *slot = value;
            queue.updateWriteIndex();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::jthread consumer([&] {
        int expected = 0;
        while (!producer_done.load(std::memory_order_acquire) || queue.size() != 0) {
            const int* slot = queue.getNextToRead();
            if (slot == nullptr) {
                std::this_thread::yield();
                continue;
            }
            CHECK(*slot == expected);
            ++expected;
            consumed.fetch_add(1, std::memory_order_relaxed);
            queue.updateReadIndex();
        }
        CHECK(expected == message_count);
    });

    producer.join();
    consumer.join();
    CHECK(consumed.load(std::memory_order_relaxed) == message_count);
}

void testMessageContract() {
    Exchange::MarketUpdate snapshot;
    snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    snapshot.depth_snapshot_.last_price_ = 1234;
    snapshot.depth_snapshot_.bids_[0] = {1233, 10};
    snapshot.depth_snapshot_.asks_[0] = {1235, 12};
    CHECK(snapshot.toString().find("DEPTH_SNAPSHOT") != std::string::npos);

    Exchange::ClientRequest request;
    request.type_ = Exchange::ClientRequestType::NEW;
    request.order_type_ = Common::OrderType::LIMIT;
    request.offset_ = Common::OrderOffset::OPEN;
    CHECK(request.toString().find("LIMIT") != std::string::npos);

    Exchange::ClientResponse response;
    response.type_ = Exchange::ClientResponseType::REJECTED;
    CHECK(response.toString().find("REJECTED") != std::string::npos);

    Trading::OMOrder order;
    order.order_type_ = Common::OrderType::LIMIT;
    order.offset_ = Common::OrderOffset::CLOSE_YESTERDAY;
    order.order_state_ = Trading::OMOrderState::PENDING_NEW;
    CHECK(order.toString().find("CLOSE_YESTERDAY") != std::string::npos);
}








void testJevCaptureDisabledByDefault() {
    Trading::JevHttpCapture capture;
    CHECK(!capture.enabled());
    Trading::JevCaptureAttemptMeta meta;
    const auto context = capture.beginAttempt(meta, "{}");
    CHECK(!context.has_value());
    capture.completeAttempt(context, meta, "{}", "{}", 0, "OK", 200, 0.0, "ok", "");
}

void testJevCaptureAttemptFilesContract() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_capture_files_contract";
    std::filesystem::remove_all(output_dir);
    Trading::JevHttpCapture capture(Trading::JevCaptureConfig{output_dir.string(), "run-A"});
    CHECK(capture.enabled());
    CHECK(capture.runId() == "run-A");

    Trading::JevCaptureAttemptMeta meta;
    meta.evaluation_id_ = 12;
    meta.ticker_id_ = 0;
    meta.attempt_ = 0;
    meta.endpoint_ = "https://openrouter.ai/api/alpha/decisions";
    meta.model_ = "typesafe/jev-1.13";
    meta.timeout_ms_ = 4000;
    meta.max_retries_ = 0;
    meta.started_utc_ = Trading::JevHttpCapture::utcTimestampNow();
    const auto context = capture.beginAttempt(meta, "abc");
    CHECK(context.has_value());
    capture.completeAttempt(context, meta, "abc", "response-bytes", 0, "OK", 200, 5.5, "ok", "");

    std::ifstream request_file(context->directory_ / "request.json", std::ios::binary);
    std::string request_bytes((std::istreambuf_iterator<char>(request_file)),
                                                        std::istreambuf_iterator<char>());
    CHECK(request_bytes == "abc");
    // SHA-256 test vector for the literal string "abc".
    CHECK(Trading::JevHttpCapture::sha256Hex("abc") ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::ifstream metadata_file(context->directory_ / "metadata.json");
    nlohmann::json metadata;
    metadata_file >> metadata;
    CHECK(metadata.at("run_id") == "run-A");
    CHECK(metadata.at("evaluation_id") == 12);
    CHECK(metadata.at("request_sha256") ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(metadata.at("response_sha256") ==
                Trading::JevHttpCapture::sha256Hex("response-bytes"));
    CHECK(metadata.at("http_status") == 200);
    CHECK(metadata.at("parse_outcome") == "ok");
    CHECK(metadata.at("elapsed_ms") == 5.5);
    CHECK(metadata.contains("source_revision"));
    CHECK(metadata.contains("binary_sha256"));
    CHECK(metadata.contains("started_utc"));
    CHECK(metadata.at("role") == "live");
    CHECK(metadata.at("variant") == "");
    std::ifstream response_file(context->directory_ / "response.raw", std::ios::binary);
    std::string response_bytes((std::istreambuf_iterator<char>(response_file)),
                                                         std::istreambuf_iterator<char>());
    CHECK(response_bytes == "response-bytes");
    std::filesystem::remove_all(output_dir);
}

void testJevCaptureEvaluateFailurePath() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_capture_failure_contract";
    std::filesystem::remove_all(output_dir);
    Trading::JevHttpConfig config;
    config.endpoint_ = "http://127.0.0.1:9/";
    config.api_key_ = "test-capture-only-key";
    config.timeout_ms_ = 300;
    config.capture_ = Trading::JevCaptureConfig{output_dir.string(), "run-B"};
    Trading::JevHttpClient client(config);
    Trading::JevEvaluationState state;
    state.evaluation_id_ = 7;
    state.ticker_id_ = 0;
    state.position_.net_position_ = 0;
    state.depth_snapshot_.bids_[0] = {99, 2};
    state.depth_snapshot_.asks_[0] = {101, 3};
    const auto expected_body = client.buildRequestBody(state);
    bool threw = false;
    try {
        (void)client.evaluate(state);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    const auto run_dir = output_dir / "run-B";
    CHECK(std::filesystem::exists(run_dir));
    bool found_attempt = false;
    for (const auto& entry : std::filesystem::directory_iterator(run_dir)) {
        found_attempt = true;
        std::ifstream request_file(entry.path() / "request.json", std::ios::binary);
        std::string request_bytes((std::istreambuf_iterator<char>(request_file)),
                                                            std::istreambuf_iterator<char>());
        CHECK(request_bytes == expected_body);
        std::ifstream metadata_file(entry.path() / "metadata.json");
        nlohmann::json metadata;
        metadata_file >> metadata;
        CHECK(metadata.at("evaluation_id") == 7);
        CHECK(metadata.at("curl_outcome") != "OK");
        CHECK(metadata.at("parse_outcome").is_null());
        std::ifstream response_file(entry.path() / "response.raw", std::ios::binary);
        std::string response_bytes((std::istreambuf_iterator<char>(response_file)),
                                                             std::istreambuf_iterator<char>());
        std::ifstream metadata_stream(entry.path() / "metadata.json", std::ios::binary);
        std::string metadata_bytes((std::istreambuf_iterator<char>(metadata_stream)),
                                                             std::istreambuf_iterator<char>());
        const auto combined = request_bytes + response_bytes + metadata_bytes;
        CHECK(combined.find("test-capture-only-key") == std::string::npos);
    }
    CHECK(found_attempt);
    std::filesystem::remove_all(output_dir);
}

}  // namespace




void testTttRecordsWithSyntheticTimestamps() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_ttt_instrumentation";
    std::filesystem::remove_all(output_dir);

    Common::AsyncLogger logger(output_dir, 64, std::chrono::milliseconds(1));
    Exchange::ClientRequestLFQueue requests(32);
    Exchange::ClientResponseLFQueue responses(32);
    Exchange::MarketUpdateLFQueue market_updates(32);
    Trading::JevDecisionLFQueue decisions(32);
    Trading::TradeEngine engine(
            7, Trading::RiskLimits{10, 10, 0.0}, &requests, &responses,
            &market_updates, &decisions, &logger);
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

    CHECK(engine.processPending());

    auto* request = requests.getNextToWriteTo();
    CHECK(request != nullptr);
    request->enqueued_ns_ = past;
    requests.updateWriteIndex();

    auto* response = responses.getNextToWriteTo();
    response->type_ = Exchange::ClientResponseType::ACCEPTED;
    response->client_id_ = 7;
    response->ticker_id_ = 0;
    response->client_order_id_ = request->order_id_;
    response->published_ns_ = past;
    responses.updateWriteIndex();

    CHECK(engine.processPending());
    logger.stop();

    std::ifstream ttt_file(output_dir / "ttt-7.log");
    const std::string ttt_text((std::istreambuf_iterator<char>(ttt_file)),
                                                             std::istreambuf_iterator<char>());
    CHECK(ttt_text.find("event=ttt_market_data_consumed") != std::string::npos);
    CHECK(ttt_text.find("event=ttt_decision_consumed") != std::string::npos);
    CHECK(ttt_text.find("event=ttt_decision_to_order") != std::string::npos);
    CHECK(ttt_text.find("event=ttt_order_round_trip") != std::string::npos);
    CHECK(ttt_text.find("elapsed_ns=") != std::string::npos);

    std::filesystem::remove_all(output_dir);
}

void testTttUnstampedMessagesProduceNoRecords() {
    const auto output_dir = std::filesystem::temp_directory_path() /
                                                    "jev_quant_ttt_unstamped";
    std::filesystem::remove_all(output_dir);

    Common::AsyncLogger logger(output_dir, 64, std::chrono::milliseconds(1));
    Exchange::ClientRequestLFQueue requests(32);
    Exchange::ClientResponseLFQueue responses(32);
    Exchange::MarketUpdateLFQueue market_updates(32);
    Trading::JevDecisionLFQueue decisions(32);
    Trading::TradeEngine engine(
            7, Trading::RiskLimits{10, 10, 0.0}, &requests, &responses,
            &market_updates, &decisions, &logger);
    logger.start();

    auto* decision = decisions.getNextToWriteTo();
    decision->evaluation_id_ = 1;
    decision->ticker_id_ = 0;
    decision->bias_ = Trading::JevBias::LONG;
    decision->intent_ = Trading::JevIntent::OPEN;
    decisions.updateWriteIndex();
    engine.registerEvaluation(0, 1);
    CHECK(engine.processPending());

    logger.stop();

    // Unstamped messages must never produce latency records (drop records
    // carry no elapsed_ns and are the only ttt_ lines expected here).
    std::ifstream ttt_file(output_dir / "ttt-7.log");
    const std::string ttt_text((std::istreambuf_iterator<char>(ttt_file)),
                                                             std::istreambuf_iterator<char>());
    CHECK(ttt_text.find("elapsed_ns=") == std::string::npos);

    std::filesystem::remove_all(output_dir);
}

int main() {
    testAsyncLoggerFixedBufferContract();
    testAsyncLoggerContract();
    testAsyncLoggerQueueOverflow();
    testAsyncLoggerLateRegistrationAndStartupVisibility();
    testAsyncLoggerWorkerBinding();
    testQueueSpscConcurrency();
    testTttRecordsWithSyntheticTimestamps();
    testTttUnstampedMessagesProduceNoRecords();
    testMessageContract();
    testJevCaptureDisabledByDefault();
    testJevCaptureAttemptFilesContract();
    testJevCaptureEvaluateFailurePath();
    return 0;
}
