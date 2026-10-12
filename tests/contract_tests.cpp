#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/lf_queue.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/jev_decision.h"
#include "trading/strategy/jev_http_client.h"
#include "trading/strategy/jev_worker.h"
#include "trading/strategy/market_order_book.h"
#include "trading/strategy/om_order.h"
#include "trading/strategy/trade_engine.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"
#include "simulated_venue.h"
namespace {

[[noreturn]] void checkFailure(const char* condition, const char* file,
                                                                int line) {
    std::cerr << "check failed: " << condition << " (" << file << ":" << line
                        << ")\n";
    std::abort();
}

#define CHECK(condition)                                                    \
    ((condition) ? static_cast<void>(0)                                       \
                               : checkFailure(#condition, __FILE__, __LINE__))

void testMessageContract() {
    Exchange::MarketUpdate snapshot;
    snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    snapshot.depth_snapshot_.last_price_ = 1234;
    snapshot.depth_snapshot_.bids_[0] = {1233, 10};
    snapshot.depth_snapshot_.asks_[0] = {1235, 12};
    CHECK(snapshot.toString().find("DEPTH_SNAPSHOT") != std::string::npos);

    Exchange::ClientRequest request;
    request.type_ = Exchange::ClientRequestType::NEW;
    request.order_type_ = Common::OrderType::LIMIT;
    request.offset_ = Common::OrderOffset::OPEN;
    CHECK(request.toString().find("LIMIT") != std::string::npos);

    Exchange::ClientResponse response;
    response.type_ = Exchange::ClientResponseType::REJECTED;
    CHECK(response.toString().find("REJECTED") != std::string::npos);

    Trading::OMOrder order;
    order.order_type_ = Common::OrderType::LIMIT;
    order.offset_ = Common::OrderOffset::CLOSE_YESTERDAY;
    order.order_state_ = Trading::OMOrderState::PENDING_NEW;
    CHECK(order.toString().find("CLOSE_YESTERDAY") != std::string::npos);
}








}  // namespace




int main() {
    testMessageContract();
    return 0;
}
