#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/lf_queue.h"
#include "common/async_logger.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"
#include "trading/strategy/jev_config.h"
#include "trading/strategy/jev_http_client.h"
#include "trading/strategy/jev_worker.h"
#include "trading/strategy/trade_engine.h"
#include "trading/venue/venue_adapter.h"
#ifdef JEV_ENABLE_SIMEX
#include "trading/venue/simex_venue_adapter.h"
#endif

namespace {
std::atomic<bool> stopping{false};

extern "C" void onSignal(int signal_number) noexcept {
    if (stopping.exchange(true, std::memory_order_acq_rel)) {
        std::signal(signal_number, SIG_DFL);
        std::raise(signal_number);
    }
}

auto readJson(const std::string& path) -> nlohmann::json {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open config file: " + path);
    nlohmann::json value;
    file >> value;
    return value;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string config_path = argc > 1 ? argv[1] : "config.json";
        const auto document = readJson(config_path);
        const auto runtime = document.value("runtime", nlohmann::json::object());
        const auto risk_json = document.value("risk", nlohmann::json::object());
        const auto instrument = runtime.value("instrument", nlohmann::json::object());
        const auto client_id = runtime.value("client_id", 1U);
        const auto ticker_id = instrument.value("ticker_id", 0U);
        const auto interval_ms = runtime.value("evaluation_interval_ms", 2000U);
        const auto order_qty = runtime.value("decision_order_quantity", 1U);
        const auto queue_capacity = runtime.value("queue_capacity", 1024U);
        const auto logging = document.value("logging", nlohmann::json::object());
        const auto log_directory = logging.value("output_directory", std::string("logs"));
        const auto verbose_market_data = logging.value("verbose_market_data", false);
        const auto venue_json = document.value("venue", nlohmann::json::object());
        const auto venue_type = venue_json.value("type", std::string("binance"));
        const bool simex = venue_type == "simex";
        const auto run_seconds = runtime.value("run_seconds", 0);

        if (ticker_id >= Common::ME_MAX_TICKERS || queue_capacity == 0 ||
            client_id == Common::ClientId_INVALID || run_seconds < 0 ||
            (simex && (interval_ms == 0 || run_seconds > 86400))) {
            throw std::runtime_error("Invalid runtime instrument or queue capacity");
        }

        Common::AsyncLogger logger(log_directory, queue_capacity,
                                   std::chrono::milliseconds(1));

        // initializing the lock-free queues
        Exchange::ClientRequestLFQueue client_requests(queue_capacity);
        Exchange::ClientResponseLFQueue client_responses(queue_capacity);
        Exchange::MarketUpdateLFQueue market_updates(queue_capacity);
        Trading::JevEvaluationStateLFQueue evaluations(queue_capacity);
        Trading::JevDecisionLFQueue decisions(queue_capacity);

        Trading::RiskLimits limits{
                risk_json.value("max_order_size", Common::Qty_INVALID),
                risk_json.value("max_position", Common::Qty_INVALID),
                risk_json.value("max_loss", 0.0)};
        std::atomic<std::uint64_t> valid_decisions{0}, executions{0};
        std::atomic<bool> market_ready{false};
        Trading::TradeEngine engine(client_id, limits, &client_requests,
                                                                &client_responses, &market_updates, &decisions,
                                                                &logger);
        engine.setDecisionOrderQuantity(order_qty);
        engine.setVerboseMarketData(verbose_market_data);
        if (simex) {
            engine.enableSimexConstraints();
            const auto unknown = std::numeric_limits<double>::quiet_NaN();
            engine.setAccountState({unknown, unknown, unknown});
            engine.onMarketUpdate = [&](const auto&, const auto& book) {
                const auto* bbo = book.getBBO();
                if (bbo->bid_price_ > 0 && bbo->ask_price_ > bbo->bid_price_ &&
                    bbo->ask_price_ != Common::Price_INVALID && bbo->bid_qty_ > 0 &&
                    bbo->bid_qty_ != Common::Qty_INVALID && bbo->ask_qty_ > 0 && bbo->ask_qty_ != Common::Qty_INVALID)
                    market_ready.store(true);
            };
            engine.onJevDecision = [&](const auto& decision) {
                ++valid_decisions;
                std::clog << "event=live_jev_decision evaluation_id=" << decision.evaluation_id_
                          << " intent=" << static_cast<int>(decision.intent_) << '\n';
            };
            engine.onClientResponse = [&](const auto& response) {
                if (response.type_ == Exchange::ClientResponseType::FILLED && response.exec_qty_ > 0) {
                    ++executions;
                    std::clog << "event=live_position_updated order_id=" << response.client_order_id_
                              << " position=" << engine.positionKeeper().getPositionInfo(response.ticker_id_).position_ << '\n';
                }
            };
        }
        engine.attachJevEvaluationQueue(&evaluations,
                                                                          std::chrono::milliseconds(interval_ms), ticker_id);

        Trading::MarketDataConsumer market_data(client_id, &market_updates, &logger,
                                                 verbose_market_data);
        Trading::OrderGateway order_gateway(client_id, &client_requests, &client_responses,
                                             &logger);
        std::unique_ptr<Trading::VenueAdapter> venue;
        if (venue_type == "binance") {
            auto config = Trading::BinanceUmFuturesVenueAdapter::loadConfigFromEnv(".env", ticker_id);
            venue = std::make_unique<Trading::BinanceUmFuturesVenueAdapter>(&market_data, &order_gateway, std::move(config), &logger);
        } else if (simex) {
#ifdef JEV_ENABLE_SIMEX
            venue = std::make_unique<Trading::SimexVenueAdapter>(&market_data, &order_gateway,
                    Trading::SimexVenueConfig::fromJson(venue_json, ticker_id));
#else
            throw std::runtime_error("Simex support was disabled at build time");
#endif
        } else throw std::runtime_error("Unknown venue type");
        auto jev_config = Trading::loadJevHttpConfig(".env", config_path);
        Trading::JevHttpClient jev_client(std::move(jev_config));
        Trading::JevWorker jev_worker(&evaluations, &decisions, &jev_client, &logger);
        if (simex) jev_worker.setEvaluationFilter([&](const auto& state) { return engine.acceptsEvaluation(state); });

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        logger.start();
        std::clog << "Starting Jev worker\n";
        jev_worker.start();
        std::clog << "Starting trade engine\n";
        engine.start();
        std::clog << "Starting " << venue_type << " venue adapter\n";
        venue->start();
        std::clog << venue_type << " venue adapter connected\n";
        std::clog << "jev_trading running\n";

        const auto started_at = std::chrono::steady_clock::now();
        while (!stopping.load(std::memory_order_acquire) &&
               (run_seconds == 0 || std::chrono::steady_clock::now() - started_at < std::chrono::seconds(run_seconds))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::clog << "Stopping " << venue_type << " venue adapter\n";
        venue->stop();
        std::clog << "Stopping trade engine\n";
        engine.stop();
        std::clog << "Stopping Jev worker\n";
        jev_worker.stop();
        logger.stop();
        if (simex) {
            std::clog << "event=simex_live_result market_ready=" << market_ready.load()
                      << " decisions=" << valid_decisions.load() << " executions=" << executions.load()
                      << " provider_failures=" << jev_worker.failureCount() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jev_trading startup failure: " << error.what() << '\n';
        return 1;
    }
}
