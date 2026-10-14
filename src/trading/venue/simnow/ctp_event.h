#pragma once
#include <variant>
#include "ThostFtdcUserApiStruct.h"
#include "common/performance_trace.h"

namespace Trading::Ctp {
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
    Common::PerformanceTrace trace{};
};

}  // namespace Trading::Ctp
