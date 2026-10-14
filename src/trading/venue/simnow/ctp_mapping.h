#pragma once
#include "ctp_event.h"
#include "common/types.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace Trading::Ctp {
inline const char* stage(Kind kind) {
    switch(kind) {
    case Kind::Auth: return "authenticate";
    case Kind::Login: return "trader_login";
    case Kind::MarketLogin: return "market_login";
    case Kind::Settlement: return "settlement_confirmation";
    case Kind::Instrument: return "instrument_query";
    case Kind::Account: return "account_query";
    case Kind::Position: return "position_query";
    case Kind::OrderQuery: return "order_query";
    case Kind::Subscription: return "market_subscription";
    default: return "unknown";
    }
}
template<std::size_t N> void put(char (&out)[N], const std::string& value) {
    if (value.size() >= N) throw std::invalid_argument("CTP field exceeds SDK width");
    std::memcpy(out,value.c_str(),value.size()+1);
}

Common::Price price(double value);
double decimal(Common::Price value);
}  // namespace Trading::Ctp
