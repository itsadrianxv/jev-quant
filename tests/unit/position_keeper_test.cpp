#include <gtest/gtest.h>

#include "common/types.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/position_keeper.h"

namespace {

using Common::OrderOffset;
using Common::PositionDay;
using Common::Side;
using Common::Qty;

Exchange::ClientResponse makeFill(Side side, OrderOffset offset, Common::Price price,
                                  Qty qty) {
    Exchange::ClientResponse response;
    response.type_ = Exchange::ClientResponseType::FILLED;
    response.ticker_id_ = 0;
    response.side_ = side;
    response.price_ = price;
    response.exec_qty_ = qty;
    response.leaves_qty_ = 0;
    response.offset_ = offset;
    return response;
}

Exchange::ClientResponse makeOpenFill(Side side, Common::Price price, Qty qty) {
    return makeFill(side, OrderOffset::OPEN, price, qty);
}

void seedPositionDay(Trading::PositionInfo& info, PositionDay day, Qty qty,
                     double vwap) {
    info.position_days_[static_cast<std::size_t>(day)] = {qty, vwap};
}

TEST(PositionInfoTest, FreshPositionHasNoCloseableQuantity) {
    Trading::PositionInfo info;

    EXPECT_EQ(0, info.position_);
    EXPECT_EQ(0u, info.todayQty());
    EXPECT_EQ(0u, info.yesterdayQty());
    EXPECT_EQ(Common::Price_INVALID, info.averageEntryPrice());
    EXPECT_EQ(0u, info.closeableQty(OrderOffset::CLOSE));
    EXPECT_EQ(0u, info.closeableQty(OrderOffset::OPEN));
    EXPECT_EQ(OrderOffset::INVALID, info.closeOffsetFor(1));
    EXPECT_DOUBLE_EQ(0.0, info.realized_pnl_);
    EXPECT_DOUBLE_EQ(0.0, info.unrealized_pnl_);
    EXPECT_DOUBLE_EQ(0.0, info.total_pnl_);
}

TEST(PositionInfoTest, OpenBuyFillsAccumulatePositionAndVwap) {
    Trading::PositionInfo info;

    info.onFill(makeOpenFill(Side::BUY, 100, 2));
    EXPECT_EQ(2, info.position_);
    EXPECT_EQ(2u, info.todayQty());
    EXPECT_EQ(100, info.averageEntryPrice());

    info.onFill(makeOpenFill(Side::BUY, 104, 2));
    EXPECT_EQ(4, info.position_);
    EXPECT_EQ(4u, info.todayQty());
    EXPECT_EQ(102, info.averageEntryPrice());
}

TEST(PositionInfoTest, OpenSellFillBuildsShortPosition) {
    Trading::PositionInfo info;

    info.onFill(makeOpenFill(Side::SELL, 50, 3));

    EXPECT_EQ(-3, info.position_);
    EXPECT_EQ(3u, info.todayQty());
    EXPECT_EQ(50, info.averageEntryPrice());
}

TEST(PositionInfoTest, CloseAllocatesTheYesterdayPositionFirst) {
    Trading::PositionInfo info;
    info.position_ = 3;
    seedPositionDay(info, PositionDay::YESTERDAY, 2, 95.0);
    seedPositionDay(info, PositionDay::TODAY, 1, 100.0);

    EXPECT_EQ(OrderOffset::CLOSE_YESTERDAY, info.closeOffsetFor(1));

    info.onFill(makeFill(Side::SELL, OrderOffset::CLOSE_YESTERDAY, 97, 2));

    EXPECT_EQ(1, info.position_);
    EXPECT_EQ(0u, info.yesterdayQty());
    EXPECT_EQ(1u, info.todayQty());
    EXPECT_DOUBLE_EQ(4.0, info.realized_pnl_);
    EXPECT_DOUBLE_EQ(4.0, info.total_pnl_);
}

TEST(PositionInfoTest, GenericCloseConsumesYesterdayThenToday) {
    Trading::PositionInfo info;
    info.position_ = 3;
    seedPositionDay(info, PositionDay::YESTERDAY, 2, 95.0);
    seedPositionDay(info, PositionDay::TODAY, 1, 100.0);

    info.onFill(makeFill(Side::SELL, OrderOffset::CLOSE, 98, 3));

    EXPECT_EQ(0, info.position_);
    EXPECT_EQ(0u, info.yesterdayQty());
    EXPECT_EQ(0u, info.todayQty());
    EXPECT_DOUBLE_EQ(4.0, info.realized_pnl_);
}

TEST(PositionInfoTest, CloseTodayTouchesOnlyTheTodayPosition) {
    Trading::PositionInfo info;
    info.position_ = 4;
    seedPositionDay(info, PositionDay::YESTERDAY, 2, 95.0);
    seedPositionDay(info, PositionDay::TODAY, 2, 90.0);

    info.onFill(makeFill(Side::SELL, OrderOffset::CLOSE_TODAY, 88, 1));

    EXPECT_EQ(3, info.position_);
    EXPECT_EQ(2u, info.yesterdayQty());
    EXPECT_EQ(1u, info.todayQty());
    EXPECT_DOUBLE_EQ(-2.0, info.realized_pnl_);
}

TEST(PositionInfoTest, ZeroOrInvalidExecQuantityIsIgnored) {
    Trading::PositionInfo info;

    info.onFill(makeOpenFill(Side::BUY, 100, 0));
    info.onFill(makeOpenFill(Side::BUY, 100, Common::Qty_INVALID));

    EXPECT_EQ(0, info.position_);
    EXPECT_EQ(0u, info.todayQty());
}

TEST(PositionInfoTest, UnrealizedPnlUsesTheBboMidPriceForLongs) {
    Trading::PositionInfo info;
    info.onFill(makeOpenFill(Side::BUY, 100, 2));
    Trading::BBO bbo;
    bbo.bid_price_ = 109;
    bbo.ask_price_ = 111;

    info.updateBBO(&bbo);

    EXPECT_DOUBLE_EQ(20.0, info.unrealized_pnl_);
    EXPECT_DOUBLE_EQ(20.0, info.total_pnl_);
}

TEST(PositionInfoTest, UnrealizedPnlUsesTheBboMidPriceForShorts) {
    Trading::PositionInfo info;
    info.onFill(makeOpenFill(Side::SELL, 100, 2));
    Trading::BBO bbo;
    bbo.bid_price_ = 90;
    bbo.ask_price_ = 92;

    info.updateBBO(&bbo);

    EXPECT_DOUBLE_EQ(18.0, info.unrealized_pnl_);
    EXPECT_DOUBLE_EQ(18.0, info.total_pnl_);
}

TEST(PositionInfoTest, FlatPositionHasZeroUnrealizedPnl) {
    Trading::PositionInfo info;
    Trading::BBO bbo;
    bbo.bid_price_ = 109;
    bbo.ask_price_ = 111;

    info.updateBBO(&bbo);

    EXPECT_DOUBLE_EQ(0.0, info.unrealized_pnl_);
    EXPECT_DOUBLE_EQ(0.0, info.total_pnl_);
}

TEST(PositionKeeperTest, NonFilledResponsesAreIgnored) {
    Trading::PositionKeeper keeper;
    Exchange::ClientResponse accepted;
    accepted.type_ = Exchange::ClientResponseType::ACCEPTED;
    accepted.ticker_id_ = 0;
    accepted.side_ = Side::BUY;
    accepted.price_ = 100;
    accepted.exec_qty_ = 2;
    accepted.offset_ = OrderOffset::OPEN;

    keeper.onClientResponse(accepted);

    EXPECT_EQ(0, keeper.getPositionInfo(0).position_);
}

TEST(PositionKeeperTest, FilledResponsesAccrueTheTickerPosition) {
    Trading::PositionKeeper keeper;

    keeper.onClientResponse(makeOpenFill(Side::BUY, 100, 2));

    EXPECT_EQ(2, keeper.getPositionInfo(0).position_);
    EXPECT_EQ(2u, keeper.getPositionInfo(0).todayQty());
}

}  // namespace