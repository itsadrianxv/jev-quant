#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/lf_queue.h"
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

namespace {
std::atomic<bool> stopping{false};

extern "C" void onSignal(int) noexcept { stopping.store(true, std::memory_order_release); }

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

        if (ticker_id >= Common::ME_MAX_TICKERS || queue_capacity == 0) {
            throw std::runtime_error("Invalid runtime instrument or queue capacity");
        }

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
        Trading::TradeEngine engine(client_id, limits, &client_requests,
                                                                &client_responses, &market_updates, &decisions);
        engine.setDecisionOrderQuantity(order_qty);
        engine.attachJevEvaluationQueue(&evaluations,
                                                                          std::chrono::milliseconds(interval_ms), ticker_id);

        Trading::MarketDataConsumer market_data(client_id, &market_updates);
        Trading::OrderGateway order_gateway(client_id, &client_requests, &client_responses);
        auto binance_config = Trading::BinanceUmFuturesVenueAdapter::loadConfigFromEnv(".env", ticker_id);
        Trading::BinanceUmFuturesVenueAdapter venue(&market_data, &order_gateway, std::move(binance_config));
        auto jev_config = Trading::loadJevHttpConfig(".env", config_path);
        Trading::JevHttpClient jev_client(std::move(jev_config));
        Trading::JevWorker jev_worker(&evaluations, &decisions, &jev_client);

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        std::clog << "Starting Jev worker\n";
        jev_worker.start();
        std::clog << "Starting trade engine\n";
        engine.start();
        std::clog << "Starting Binance UM Futures demo venue adapter\n";
        venue.start();
        std::clog << "Binance UM Futures demo venue adapter ready\n";
        std::clog << "jev_trading running\n";

        while (!stopping.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::clog << "Stopping Binance UM Futures demo venue adapter\n";
        venue.stop();
        std::clog << "Stopping trade engine\n";
        engine.stop();
        std::clog << "Stopping Jev worker\n";
        jev_worker.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "jev_trading startup failure: " << error.what() << '\n';
        return 1;
    }
}
