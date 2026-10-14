#pragma once
#include "ctp_transport.h"
#include "simnow_control.h"
#include "trading/market_data/market_data_consumer.h"

namespace Trading {
// Mutable market state belongs to the MarketDataConsumer worker.
class SimNowMarketData {
  public:
    SimNowMarketData(MarketDataConsumer* market, const SimNowConfig& config,
                     Ctp::Transport* transport, SimNowControl& control);
    void enqueue(Ctp::Event event) noexcept;
    void pollMarket() noexcept;

  private:
    using Kind = Ctp::Kind;
    MarketDataConsumer* market;
    const SimNowConfig& config;
    Ctp::Transport* transport;
    SimNowControl& control;
    Common::LFQueue<Ctp::Event> market_events;
    bool market_connected = false;
    int market_next_id = 0, market_login_request = 0;
    void depth(const CThostFtdcDepthMarketDataField& p, const Common::PerformanceTrace& trace);
    void marketRequest(Kind kind);
    void marketEvent(const Ctp::Event& e);
};
}  // namespace Trading
