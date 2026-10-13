#include <gtest/gtest.h>

#include <string>

#include "common/types.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/om_order.h"

namespace {

// A depth snapshot's toString must render the update type so the snapshot is
// identifiable in logs.
TEST(MessageRenderingTest, DepthSnapshotToStringCarriesTheUpdateType) {
    Exchange::MarketUpdate snapshot;
    snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    snapshot.depth_snapshot_.last_price_ = 1234;
    snapshot.depth_snapshot_.bids_[0] = {1233, 10};
    snapshot.depth_snapshot_.asks_[0] = {1235, 12};

    EXPECT_NE(std::string::npos, snapshot.toString().find("DEPTH_SNAPSHOT"));
}

// A client request's toString must render the order type (LIMIT) so the
// request is identifiable in logs.
TEST(MessageRenderingTest, ClientRequestToStringCarriesTheOrderType) {
    Exchange::ClientRequest request;
    request.type_ = Exchange::ClientRequestType::NEW;
    request.order_type_ = Common::OrderType::LIMIT;
    request.offset_ = Common::OrderOffset::OPEN;

    EXPECT_NE(std::string::npos, request.toString().find("LIMIT"));
}

// A client response's toString must render the response type (REJECTED) so
// the response is identifiable in logs.
TEST(MessageRenderingTest, ClientResponseToStringCarriesTheResponseType) {
    Exchange::ClientResponse response;
    response.type_ = Exchange::ClientResponseType::REJECTED;

    EXPECT_NE(std::string::npos, response.toString().find("REJECTED"));
}

// An OMOrder's toString must render the order offset (CLOSE_YESTERDAY) so
// the order state is identifiable in logs.
TEST(MessageRenderingTest, OmOrderToStringCarriesTheOrderOffset) {
    Trading::OMOrder order;
    order.order_type_ = Common::OrderType::LIMIT;
    order.offset_ = Common::OrderOffset::CLOSE_YESTERDAY;
    order.order_state_ = Trading::OMOrderState::PENDING_NEW;

    EXPECT_NE(std::string::npos, order.toString().find("CLOSE_YESTERDAY"));
}

}  // namespace
