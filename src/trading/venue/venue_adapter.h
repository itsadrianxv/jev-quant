#pragma once

#include <string>

namespace Trading {

class VenueAdapter {
  public:
    virtual ~VenueAdapter() = default;
    /// Prepare transport resources and callbacks without starting business workers.
    /// The owner then starts MarketDataConsumer and OrderGateway explicitly.
    virtual auto prepare() -> void = 0;
    /// Called after both workers start. Some venues need them to finish login.
    virtual auto waitUntilReady() -> void {}
    /// Finish venue shutdown work while the gateway can still process responses.
    virtual auto quiesce() -> void {}
    /// Release transport and callbacks only after the owner joins both workers.
    /// The same ordering is required before destruction, including on exceptions.
    virtual auto stop() -> void = 0;
    /// Queries recorded failures only; false does not certify venue health.
    /// Fatal errors in some adapters still terminate rather than recording a failure.
    [[nodiscard]] virtual bool failed() const noexcept { return false; }
    [[nodiscard]] virtual std::string failure() const { return {}; }
    [[nodiscard]] virtual auto running() const noexcept -> bool = 0;
};

}  // namespace Trading
