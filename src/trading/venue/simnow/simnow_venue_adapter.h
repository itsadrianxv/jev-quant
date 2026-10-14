#pragma once
#include <functional>
#include <memory>
#include "simnow_config.h"
#include "trading/venue/venue_adapter.h"
#include "trading/strategy/jev_decision.h"

namespace Trading {
class MarketDataConsumer;
class OrderGateway;
namespace Ctp { class Transport; }

class SimNowVenueAdapter final : public VenueAdapter {
  public:
    using StateSink = std::function<void(bool, AccountState, double)>;
    SimNowVenueAdapter(MarketDataConsumer*, OrderGateway*, SimNowConfig,
                      StateSink);
    /// Internal transport injection for offline adapter tests.
    SimNowVenueAdapter(MarketDataConsumer*, OrderGateway*, SimNowConfig,
                      StateSink, std::unique_ptr<Ctp::Transport>);
    ~SimNowVenueAdapter() override;
    void prepare() override;
    void waitUntilReady() override;
    void quiesce() override;
    void stop() override;
    bool running() const noexcept override;
    bool failed() const noexcept override;
    std::string failure() const override;
    int position() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
