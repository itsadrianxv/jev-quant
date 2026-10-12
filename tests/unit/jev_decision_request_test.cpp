#include <gtest/gtest.h>

#include <cstdint>

#include <nlohmann/json.hpp>
#include <string>

#include "common/types.h"
#include "trading/strategy/jev_http_client.h"
#include "trading/strategy/om_order.h"

namespace {

using Json = nlohmann::json;

Trading::JevEvaluationState makeState() {
    Trading::JevEvaluationState state;
    state.evaluation_id_ = 9;
    state.ticker_id_ = 0;
    return state;
}

Json buildRequest(const Trading::JevEvaluationState& state) {
    const Trading::JevHttpClient client(Trading::JevHttpConfig{});
    return Json::parse(client.buildRequestBody(state));
}

TEST(JevHttpRequestBodyTest, FlatPositionSelectsTheOpenAndHoldCriteria) {
    const Json request = buildRequest(makeState());

    EXPECT_EQ("typesafe/jev-1.13", request.at("model").get<std::string>());
    const auto& criteria = request.at("questions").at("intent").at("criteria");
    EXPECT_EQ("open a position", criteria.at("open").get<std::string>());
    EXPECT_EQ("take no action", criteria.at("hold").get<std::string>());
    EXPECT_FALSE(criteria.contains("close"));
}

TEST(JevHttpRequestBodyTest, FlatPositionShipsNullVolumeAndLimitPrices) {
    const Json request = buildRequest(makeState());

    const auto& depth = request.at("state").at("depth");
    EXPECT_TRUE(depth.at("volume").is_null());
    EXPECT_TRUE(depth.at("upper_limit_price").is_null());
    EXPECT_TRUE(depth.at("lower_limit_price").is_null());
}

TEST(JevHttpRequestBodyTest, OccupiedPositionSelectsTheIncreaseAndCloseCriteria) {
    auto state = makeState();
    state.position_.net_position_ = 2;

    const Json request = buildRequest(state);

    const auto& criteria = request.at("questions").at("intent").at("criteria");
    EXPECT_EQ("increase exposure", criteria.at("open").get<std::string>());
    EXPECT_EQ("take no action", criteria.at("hold").get<std::string>());
    const auto& close = criteria.at("close");
    EXPECT_TRUE(close.is_string());
    EXPECT_NE(std::string::npos,
              close.get<std::string>().find("Reduce the current position"));
}

TEST(JevHttpRequestBodyTest, LiveWorkingOrdersShipTheirAttributes) {
    auto state = makeState();
    auto& order = state.working_orders_[0];
    order.order_id_ = 12;
    order.side_ = Common::Side::BUY;
    order.price_ = 100;
    order.qty_ = 1;
    order.order_type_ = Common::OrderType::LIMIT;
    order.offset_ = Common::OrderOffset::OPEN;
    order.state_ = Trading::OMOrderState::LIVE;

    const Json request = buildRequest(state);

    const auto& orders = request.at("state").at("working_orders");
    ASSERT_TRUE(orders.is_array());
    ASSERT_EQ(1u, orders.size());
    const auto& shipped = orders.at(0);
    EXPECT_EQ(12, shipped.at("order_id"));
    EXPECT_EQ("BUY", shipped.at("side").get<std::string>());
    EXPECT_EQ(100, shipped.at("price"));
    EXPECT_EQ(1, shipped.at("qty"));
    EXPECT_EQ("LIMIT", shipped.at("order_type").get<std::string>());
    EXPECT_EQ("OPEN", shipped.at("offset").get<std::string>());
    EXPECT_EQ("LIVE", shipped.at("state").get<std::string>());
}

TEST(JevHttpRequestBodyTest, RequestCarriesEvaluationIdAndOmitsTheEndpoint) {
    const Json request = buildRequest(makeState());

    EXPECT_EQ(9u, request.at("state").at("evaluation_id").get<std::uint64_t>());
    EXPECT_FALSE(request.contains("endpoint"));
}

TEST(JevHttpRequestBodyTest, EvaluationStateShipsScaledFeaturesAndEssentialKeys) {
    auto state = makeState();
    state.price_scale_ = 100.0;
    state.market_features_.valid_ = true;
    state.market_features_.mid_price_ = 100.5;
    state.market_features_.relative_spread_bps_ = 10000.0 / 100.5;
    state.market_features_.top_of_book_total_qty_ = 8;
    state.market_features_.top_of_book_qty_imbalance_ = -0.5;

    const Json request = buildRequest(state);
    const auto& shipped = request.at("state");

    EXPECT_TRUE(shipped.contains("position"));
    EXPECT_TRUE(shipped.contains("risk"));
    EXPECT_TRUE(shipped.contains("account"));

    const auto& depth = shipped.at("depth");
    EXPECT_FALSE(depth.contains("bids"));
    EXPECT_FALSE(depth.contains("asks"));
    EXPECT_TRUE(depth.contains("last_price"));

    const auto& features = shipped.at("features");
    EXPECT_DOUBLE_EQ(1.005, features.at("mid_price").get<double>());
    EXPECT_EQ(8u, features.at("top_of_book_total_qty").get<std::uint64_t>());
    EXPECT_DOUBLE_EQ(-0.5, features.at("top_of_book_qty_imbalance").get<double>());
    EXPECT_NEAR(10000.0 / 100.5, features.at("relative_spread_bps").get<double>(), 1e-9);
}

TEST(JevHttpRequestBodyTest, DeadWorkingOrdersAreOmitted) {
    auto state = makeState();
    state.working_orders_[0].order_id_ = 12;
    state.working_orders_[0].state_ = Trading::OMOrderState::DEAD;

    const Json request = buildRequest(state);

    const auto& orders = request.at("state").at("working_orders");
    EXPECT_TRUE(orders.is_array());
    EXPECT_TRUE(orders.empty());
}

}  // namespace
