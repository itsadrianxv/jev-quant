#pragma once

#include <chrono>
#include <memory>
#include <string>
#include "trading/venue/venue_adapter.h"

namespace Trading {

struct SimexVenueConfig {
    std::string host = "127.0.0.1";
    std::uint16_t tcp_port = 19001;
    std::uint16_t udp_port = 19002;
    Common::TickerId ticker_id = 0;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds feed_timeout{5000};
    std::size_t capacity = 4096;
    static auto fromJson(const nlohmann::json& value, Common::TickerId ticker) -> SimexVenueConfig;
};

// The OrderGateway worker owns all socket and mapping state.
class SimexVenueAdapter final : public VenueAdapter {
  public:
    SimexVenueAdapter(MarketDataConsumer* market, OrderGateway* orders, SimexVenueConfig config);
    ~SimexVenueAdapter() override;
    auto start() -> void override;
    auto stop() -> void override;
    [[nodiscard]] auto running() const noexcept -> bool override;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Trading
