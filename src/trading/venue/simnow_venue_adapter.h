#pragma once

#include <chrono>
#include <functional>
#include <variant>
#include "ThostFtdcUserApiStruct.h"
#include "trading/venue/venue_adapter.h"
#include "trading/strategy/jev_decision.h"

namespace Trading {

struct SimNowConfig {
    std::string trading_front, market_front, instrument;
    std::string broker = "9999", app_id = "simnow_client_test", auth_code;
    std::string user, password, flow_directory = "flows/simnow";
    Common::TickerId ticker_id = 0;
    std::size_t capacity = 4096;
    std::chrono::milliseconds startup_timeout{60000}, shutdown_timeout{10000};
    std::chrono::milliseconds query_interval{1000};
    static SimNowConfig fromJson(const nlohmann::json&, Common::TickerId,
                                const std::string& dotenv_path = ".env");
};

namespace Ctp {
enum class Kind { TraderConnected, MarketConnected, Disconnected, Auth, Login,
    MarketLogin, Settlement, Instrument, Account, Position, OrderQuery,
    Subscription, Depth, Order, Trade, InsertError, CancelError, Error };
using Payload = std::variant<std::monostate, CThostFtdcRspUserLoginField,
    CThostFtdcInstrumentField, CThostFtdcTradingAccountField,
    CThostFtdcInvestorPositionField, CThostFtdcOrderField,
    CThostFtdcTradeField, CThostFtdcDepthMarketDataField,
    CThostFtdcInputOrderField, CThostFtdcInputOrderActionField>;
struct Event {
    Kind kind;
    int request_id = 0, error = 0;
    bool last = true;
    Payload payload;
};

// Internal SDK seam: fake transports drive the same event queue in offline tests.
class Transport {
  public:
    using Sink = std::function<void(Event)>;
    virtual ~Transport() = default;
    virtual void start(const SimNowConfig&, Sink) = 0;
    virtual void stop() noexcept = 0;
    virtual int request(Kind, int) = 0;
    virtual int insert(const CThostFtdcInputOrderField&, int) = 0;
    virtual int cancel(const CThostFtdcInputOrderActionField&, int) = 0;
};
std::unique_ptr<Transport> makeTransport();
Common::Price price(double);
double decimal(Common::Price);
}

class SimNowVenueAdapter final : public VenueAdapter {
  public:
    using StateSink = std::function<void(bool, AccountState, double)>;
    SimNowVenueAdapter(MarketDataConsumer*, OrderGateway*, SimNowConfig,
                      StateSink, std::unique_ptr<Ctp::Transport> = Ctp::makeTransport());
    ~SimNowVenueAdapter() override;
    void start() override;
    void stop() override;
    bool running() const noexcept override;
    bool failed() const noexcept;
    std::string failure() const;
    int position() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
