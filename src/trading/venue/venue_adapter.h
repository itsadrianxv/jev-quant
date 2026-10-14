#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <nlohmann/json.hpp>
#include "trading/venue/binance_depth_book.h"
#include "trading/venue/binance_websocket_stream.h"
#include <string>
#include <unordered_map>
#include <optional>
#include <utility>

#include "common/async_logger.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"

namespace Trading {

class VenueAdapter {
  public:
    virtual ~VenueAdapter() = default;
    virtual auto start() -> void = 0;
    virtual auto stop() -> void = 0;
    [[nodiscard]] virtual auto running() const noexcept -> bool = 0;
};

struct BinanceUmFuturesConfig {
    std::string api_key;
    std::string secret_key;
    std::string symbol;
    std::uint32_t leverage = 1;
    std::string margin_type = "ISOLATED";
    Common::TickerId ticker_id = Common::TickerId_INVALID;
};

struct BinanceOrderParameters {
    std::string symbol;
    std::string side;
    std::string type;
    std::string quantity;
    std::string price;
    std::string time_in_force;
    bool reduce_only = false;
    std::string new_client_order_id;
};

class BinanceUmFuturesVenueAdapter final : public VenueAdapter {
  public:
    /// Transfer unbound handles; the owning logger must outlive this component.
    BinanceUmFuturesVenueAdapter(MarketDataConsumer* market_data,
                                 OrderGateway* order_gateway,
                                 BinanceUmFuturesConfig config,
                                 std::optional<Common::AsyncLogger::ProducerHandle> log_handle = std::nullopt);

    ~BinanceUmFuturesVenueAdapter() override;

    BinanceUmFuturesVenueAdapter(const BinanceUmFuturesVenueAdapter&) = delete;
    BinanceUmFuturesVenueAdapter& operator=(const BinanceUmFuturesVenueAdapter&) = delete;

    auto start() -> void override;
    auto stop() -> void override;
    [[nodiscard]] auto running() const noexcept -> bool override;

    [[nodiscard]] static auto loadConfigFromEnv(const std::string& path,
                                                Common::TickerId ticker_id)
            -> BinanceUmFuturesConfig;
    [[nodiscard]] static auto mapOrder(const Exchange::ClientRequest& request,
                                       const BinanceUmFuturesConfig& config)
            -> BinanceOrderParameters;
    [[nodiscard]] static auto mapOrderStatus(const std::string& status)
            -> Exchange::ClientResponseType;
    [[nodiscard]] static auto adjustedTimestamp(std::int64_t local_time_ms,
                                                 std::int64_t server_offset_ms)
            -> std::int64_t;

  private:
    friend class BinanceVenueHarness;

    auto pollMarket() -> void;
    auto pollOrders() -> void;
    auto handleUserEvent(const nlohmann::json& message) -> void;
    auto handleRequest(std::size_t sequence,
                       const Exchange::ClientRequest& request) -> void;
    auto publishRejected(std::size_t sequence,
                         const Exchange::ClientRequest& request,
                         const std::string& reason) -> void;

    MarketDataConsumer* market_data_ = nullptr;
    OrderGateway* order_gateway_ = nullptr;
    BinanceUmFuturesConfig config_{};
    std::atomic<bool> running_{false};
    bool lifecycle_started_ = false;
    std::int64_t server_time_offset_ms_ = 0;
    std::unique_ptr<BinanceWebSocketStream> depth_stream_;
    std::unique_ptr<BinanceWebSocketStream> user_stream_;
    std::string listen_key_;
    std::atomic<bool> keepalive_running_{false};
    std::jthread keepalive_thread_;
    nlohmann::json account_state_;
    BinanceDepthBook depth_book_;
    // Depth events go to the market worker; user events go to the gateway worker.
    // TODO: Decode directly into typed events to avoid copying JSON payloads.
    Common::LFQueue<nlohmann::json> depth_events_{1024};
    Common::LFQueue<nlohmann::json> user_events_{1024};
    bool snapshot_pending_ = false;
    std::unordered_map<Common::OrderId, Exchange::ClientRequest> live_orders_;
    std::unordered_map<Common::OrderId, Common::Qty> cumulative_exec_qty_;
    std::optional<Common::AsyncLogger::ProducerHandle> log_handle_;
};

}  // namespace Trading
