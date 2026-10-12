#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "common/types.h"
#include "exchange/order_server/client_request.h"
#include "trading/venue/venue_adapter.h"

namespace {

using Common::OrderOffset;
using Common::OrderType;
using Common::Price;
using Common::Qty;
using Common::Side;
using Exchange::ClientResponseType;

Trading::BinanceUmFuturesConfig makeConfig() {
    Trading::BinanceUmFuturesConfig config;
    config.symbol = "BTCUSDT";
    config.ticker_id = 0;
    return config;
}

Exchange::ClientRequest makeNewRequest(Side side, OrderType type,
                                       OrderOffset offset, Qty qty, Price price) {
    Exchange::ClientRequest request;
    request.type_ = Exchange::ClientRequestType::NEW;
    request.ticker_id_ = 0;
    request.order_id_ = 42;
    request.side_ = side;
    request.qty_ = qty;
    request.price_ = price;
    request.order_type_ = type;
    request.offset_ = offset;
    return request;
}

struct OrderRow {
    const char* label;
    Side side;
    OrderType type;
    OrderOffset offset;
    Qty qty;
    Price price;
    const char* expected_side;
    const char* expected_type;
    const char* expected_time_in_force;
    bool expected_reduce_only;
    const char* expected_quantity;
    const char* expected_price;
    const char* expected_client_order_id;
};

constexpr OrderRow kOrderRows[] = {
        {"LimitSellCloseYesterday", Side::SELL, OrderType::LIMIT,
         OrderOffset::CLOSE_YESTERDAY, 3, 100, "SELL", "LIMIT", "GTC", true,
         "3", "100", "42"},
        {"LimitBuyOpen", Side::BUY, OrderType::LIMIT, OrderOffset::OPEN,
         2, 99, "BUY", "LIMIT", "GTC", false, "2", "99", "42"},
        {"MarketBuyOpen", Side::BUY, OrderType::MARKET, OrderOffset::OPEN,
         1, 25000, "BUY", "MARKET", "", false, "1", "25000", "42"},
        {"MarketSellCloseToday", Side::SELL, OrderType::MARKET,
         OrderOffset::CLOSE_TODAY, 5, 101, "SELL", "MARKET", "", true,
         "5", "101", "42"},
};

class BinanceUmFuturesMapOrderTest : public ::testing::TestWithParam<OrderRow> {};

TEST_P(BinanceUmFuturesMapOrderTest, MapsTheRequestToBinanceParameters) {
    const auto& row = GetParam();
    const auto mapped = Trading::BinanceUmFuturesVenueAdapter::mapOrder(
            makeNewRequest(row.side, row.type, row.offset, row.qty, row.price),
            makeConfig());

    EXPECT_EQ("BTCUSDT", mapped.symbol);
    EXPECT_EQ(std::string(row.expected_side), mapped.side);
    EXPECT_EQ(std::string(row.expected_type), mapped.type);
    EXPECT_EQ(std::string(row.expected_time_in_force), mapped.time_in_force);
    EXPECT_EQ(row.expected_reduce_only, mapped.reduce_only);
    EXPECT_EQ(std::string(row.expected_quantity), mapped.quantity);
    EXPECT_EQ(std::string(row.expected_price), mapped.price);
    EXPECT_EQ(std::string(row.expected_client_order_id), mapped.new_client_order_id);
}

INSTANTIATE_TEST_SUITE_P(TruthTable, BinanceUmFuturesMapOrderTest,
                         ::testing::ValuesIn(kOrderRows),
                         [](const ::testing::TestParamInfo<OrderRow>& info) {
                             return info.param.label;
                         });

TEST(BinanceUmFuturesMapOrderTest, WrongTickerIsRejected) {
    auto request = makeNewRequest(Side::BUY, OrderType::LIMIT, OrderOffset::OPEN, 1, 100);
    request.ticker_id_ = 1;

    EXPECT_THROW(
            (void)Trading::BinanceUmFuturesVenueAdapter::mapOrder(request, makeConfig()),
            std::invalid_argument);
}

TEST(BinanceUmFuturesMapOrderTest, CancelRequestIsRejected) {
    auto request = makeNewRequest(Side::BUY, OrderType::LIMIT, OrderOffset::OPEN, 1, 100);
    request.type_ = Exchange::ClientRequestType::CANCEL;

    EXPECT_THROW(
            (void)Trading::BinanceUmFuturesVenueAdapter::mapOrder(request, makeConfig()),
            std::invalid_argument);
}

TEST(BinanceUmFuturesMapOrderTest, UnsupportedOrderTypeIsRejected) {
    auto request =
            makeNewRequest(Side::BUY, OrderType::INVALID, OrderOffset::OPEN, 1, 100);

    EXPECT_THROW(
            (void)Trading::BinanceUmFuturesVenueAdapter::mapOrder(request, makeConfig()),
            std::invalid_argument);
}

TEST(BinanceUmFuturesMapOrderTest, InvalidQuantityIsRejected) {
    auto request = makeNewRequest(Side::BUY, OrderType::LIMIT, OrderOffset::OPEN,
                                  Common::Qty_INVALID, 100);

    EXPECT_THROW(
            (void)Trading::BinanceUmFuturesVenueAdapter::mapOrder(request, makeConfig()),
            std::invalid_argument);
}

TEST(BinanceUmFuturesMapOrderTest, InvalidSideIsRejected) {
    auto request =
            makeNewRequest(Side::INVALID, OrderType::LIMIT, OrderOffset::OPEN, 1, 100);

    EXPECT_THROW(
            (void)Trading::BinanceUmFuturesVenueAdapter::mapOrder(request, makeConfig()),
            std::invalid_argument);
}

struct StatusRow {
    const char* label;
    const char* status;
    ClientResponseType expected;
};

constexpr StatusRow kStatusRows[] = {
        {"Filled", "FILLED", ClientResponseType::FILLED},
        {"Canceled", "CANCELED", ClientResponseType::CANCELED},
        {"Expired", "EXPIRED", ClientResponseType::CANCELED},
        {"ExpiredInMatch", "EXPIRED_IN_MATCH", ClientResponseType::CANCELED},
        {"PartiallyFilled", "PARTIALLY_FILLED", ClientResponseType::ACCEPTED},
        {"New", "NEW", ClientResponseType::ACCEPTED},
        {"UnknownStatus", "PENDING_NEW", ClientResponseType::REJECTED},
};

class BinanceUmFuturesMapOrderStatusTest
        : public ::testing::TestWithParam<StatusRow> {};

TEST_P(BinanceUmFuturesMapOrderStatusTest, MapsStatusToTheClientResponseType) {
    const auto& row = GetParam();

    EXPECT_EQ(row.expected,
              Trading::BinanceUmFuturesVenueAdapter::mapOrderStatus(row.status));
}

INSTANTIATE_TEST_SUITE_P(TruthTable, BinanceUmFuturesMapOrderStatusTest,
                         ::testing::ValuesIn(kStatusRows),
                         [](const ::testing::TestParamInfo<StatusRow>& info) {
                             return info.param.label;
                         });

struct TimestampRow {
    const char* label;
    std::int64_t local_time_ms;
    std::int64_t server_offset_ms;
    std::int64_t expected;
};

constexpr TimestampRow kTimestampRows[] = {
        {"NegativeOffset", 10000, -3600, 6400},
        {"PositiveOffset", 10000, 125, 10125},
};

class BinanceUmFuturesAdjustedTimestampTest
        : public ::testing::TestWithParam<TimestampRow> {};

TEST_P(BinanceUmFuturesAdjustedTimestampTest, AddsTheServerOffsetToTheLocalTimestamp) {
    const auto& row = GetParam();

    EXPECT_EQ(row.expected,
              Trading::BinanceUmFuturesVenueAdapter::adjustedTimestamp(
                      row.local_time_ms, row.server_offset_ms));
}

INSTANTIATE_TEST_SUITE_P(TruthTable, BinanceUmFuturesAdjustedTimestampTest,
                         ::testing::ValuesIn(kTimestampRows),
                         [](const ::testing::TestParamInfo<TimestampRow>& info) {
                             return info.param.label;
                         });

}  // namespace
