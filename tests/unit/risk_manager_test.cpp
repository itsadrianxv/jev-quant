#include <gtest/gtest.h>

#include "common/types.h"
#include "trading/strategy/position_keeper.h"
#include "trading/strategy/risk_manager.h"

namespace {

using Common::OrderOffset;
using Common::PositionDay;
using Common::Qty;
using Common::Side;
using Trading::RiskCheckResult;

struct RiskCase {
    const char* label;
    Qty qty;
    Side side;
    OrderOffset offset;
    Qty max_order_size;
    Qty max_position;
    double max_loss;
    std::int32_t position;
    Qty today_qty;
    Qty yesterday_qty;
    double total_pnl;
    RiskCheckResult expected;
};

constexpr RiskCase kRiskCases[] = {
        {"ZeroQuantityIsInvalid",
         0, Side::BUY, OrderOffset::OPEN, 10, 100, 0.0, 0, 0, 0, 0.0,
         RiskCheckResult::INVALID},
        {"SentinelQuantityIsInvalid",
         Common::Qty_INVALID, Side::BUY, OrderOffset::OPEN, 10, 100, 0.0, 0, 0, 0,
         0.0, RiskCheckResult::INVALID},
        {"InvalidSideIsInvalid",
         1, Side::INVALID, OrderOffset::OPEN, 10, 100, 0.0, 0, 0, 0, 0.0,
         RiskCheckResult::INVALID},
        {"OrderAboveLimitIsRejected",
         11, Side::BUY, OrderOffset::OPEN, 10, 100, 0.0, 0, 0, 0, 0.0,
         RiskCheckResult::ORDER_TOO_LARGE},
        {"OpenAbovePositionLimitIsRejected",
         3, Side::BUY, OrderOffset::OPEN, 10, 10, 0.0, 8, 0, 0, 0.0,
         RiskCheckResult::POSITION_TOO_LARGE},
        {"ShortOpenAbovePositionLimitIsRejected",
         3, Side::SELL, OrderOffset::OPEN, 10, 10, 0.0, -8, 0, 0, 0.0,
         RiskCheckResult::POSITION_TOO_LARGE},
        {"CloseAboveTodayQuantityIsRejected",
         3, Side::SELL, OrderOffset::CLOSE_TODAY, 10, 100, 0.0, 2, 2, 0, 0.0,
         RiskCheckResult::CLOSE_QUANTITY_TOO_LARGE},
        {"LossBeyondLimitIsRejected",
         1, Side::BUY, OrderOffset::OPEN, 10, 100, 100.0, 0, 0, 0, -105.0,
         RiskCheckResult::LOSS_TOO_LARGE},
        {"SentinelLimitsDisableEveryCap",
         1, Side::BUY, OrderOffset::OPEN, Common::Qty_INVALID, Common::Qty_INVALID,
         0.0, 50, 0, 0, 0.0, RiskCheckResult::ALLOWED},
        {"CloseWithinYesterdayQuantityIsAllowed",
         2, Side::SELL, OrderOffset::CLOSE_YESTERDAY, 10, 100, 0.0, 2, 0, 2, 0.0,
         RiskCheckResult::ALLOWED},
        {"WithinLimitsIsAllowed",
         1, Side::BUY, OrderOffset::OPEN, 10, 100, 0.0, 0, 0, 0, 0.0,
         RiskCheckResult::ALLOWED},
};

class RiskManagerCheckPreTradeRiskTest
        : public ::testing::TestWithParam<RiskCase> {};

// Table-driven: seeds the PositionKeeper with each scenario's position, day
// lots, pnl, and limits, then checks that checkPreTradeRisk returns the
// expected verdict (INVALID, ORDER_TOO_LARGE, POSITION_TOO_LARGE,
// CLOSE_QUANTITY_TOO_LARGE, LOSS_TOO_LARGE, or ALLOWED).
TEST_P(RiskManagerCheckPreTradeRiskTest, ProducesTheExpectedVerdict) {
    const RiskCase& risk_case = GetParam();
    Trading::PositionKeeper keeper;
    auto& info = keeper.getMutablePositionInfo(0);
    info.position_ = risk_case.position;
    info.total_pnl_ = risk_case.total_pnl;
    info.position_days_[static_cast<std::size_t>(PositionDay::TODAY)] = {
            risk_case.today_qty, 0.0};
    info.position_days_[static_cast<std::size_t>(PositionDay::YESTERDAY)] = {
            risk_case.yesterday_qty, 0.0};

    Trading::RiskManager manager(
            &keeper, {risk_case.max_order_size, risk_case.max_position,
                      risk_case.max_loss});

    EXPECT_EQ(risk_case.expected,
              manager.checkPreTradeRisk(0, risk_case.side, risk_case.offset,
                                        risk_case.qty));
}

INSTANTIATE_TEST_SUITE_P(TruthTable, RiskManagerCheckPreTradeRiskTest,
                         ::testing::ValuesIn(kRiskCases),
                         [](const ::testing::TestParamInfo<RiskCase>& info) {
                             return info.param.label;
                         });

// A RiskManager built without a PositionKeeper cannot validate anything and
// must fail closed with INVALID.
TEST(RiskManagerTest, NullPositionKeeperYieldsInvalid) {
    Trading::RiskManager manager(nullptr, {10, 100, 0.0});

    EXPECT_EQ(RiskCheckResult::INVALID,
              manager.checkPreTradeRisk(0, Side::BUY, OrderOffset::OPEN, 1));
}

class RiskManagerResolveCloseOffsetTest : public ::testing::Test {
  protected:
    RiskManagerResolveCloseOffsetTest() : manager_(&keeper_, {10, 100, 0.0}) {}

    Trading::PositionInfo& info() { return keeper_.getMutablePositionInfo(0); }

    Trading::PositionKeeper keeper_;
    Trading::RiskManager manager_;
};

// With both yesterday and today lots available, the close offset resolves to
// CLOSE_YESTERDAY because older positions are closed first.
TEST_F(RiskManagerResolveCloseOffsetTest, PrefersTheYesterdayPosition) {
    info().position_ = 3;
    info().position_days_[static_cast<std::size_t>(PositionDay::YESTERDAY)] = {2, 95.0};
    info().position_days_[static_cast<std::size_t>(PositionDay::TODAY)] = {1, 100.0};

    EXPECT_EQ(OrderOffset::CLOSE_YESTERDAY, manager_.resolveCloseOffset(0, 1));
}

// With no yesterday lot but an open today lot, the close offset resolves to
// CLOSE_TODAY.
TEST_F(RiskManagerResolveCloseOffsetTest, FallsBackToTheTodayPosition) {
    info().position_ = 3;
    info().position_days_[static_cast<std::size_t>(PositionDay::TODAY)] = {3, 90.0};

    EXPECT_EQ(OrderOffset::CLOSE_TODAY, manager_.resolveCloseOffset(0, 1));
}

// A flat position has nothing to close, so the offset resolution is INVALID.
TEST_F(RiskManagerResolveCloseOffsetTest, NothingCloseableIsInvalid) {
    info().position_ = 0;

    EXPECT_EQ(OrderOffset::INVALID, manager_.resolveCloseOffset(0, 1));
}

// A close request for zero quantity is not actionable and resolves to INVALID.
TEST_F(RiskManagerResolveCloseOffsetTest, ZeroRequestIsInvalid) {
    info().position_ = 2;
    info().position_days_[static_cast<std::size_t>(PositionDay::YESTERDAY)] = {2, 95.0};

    EXPECT_EQ(OrderOffset::INVALID, manager_.resolveCloseOffset(0, 0));
}

// A ticker id beyond ME_MAX_TICKERS has no position info and resolves to
// INVALID.
TEST_F(RiskManagerResolveCloseOffsetTest, OutOfRangeTickerIsInvalid) {
    EXPECT_EQ(OrderOffset::INVALID,
              manager_.resolveCloseOffset(Common::ME_MAX_TICKERS, 1));
}

class RiskManagerCloseableQuantityTest : public ::testing::Test {
  protected:
    RiskManagerCloseableQuantityTest() : manager_(&keeper_, {10, 100, 0.0}) {}

    RiskManagerCloseableQuantityTest& seed() {
        auto& info = keeper_.getMutablePositionInfo(0);
        info.position_ = 6;
        info.position_days_[static_cast<std::size_t>(PositionDay::TODAY)] = {2, 90.0};
        info.position_days_[static_cast<std::size_t>(PositionDay::YESTERDAY)] = {4, 95.0};
        return *this;
    }

    Trading::PositionKeeper keeper_;
    Trading::RiskManager manager_;
};

// OPEN and INVALID offsets are not close actions, so no quantity is closeable.
TEST_F(RiskManagerCloseableQuantityTest, OpenAndInvalidOffsetsCloseNothing) {
    seed();

    EXPECT_EQ(0u, manager_.closeableQty(0, OrderOffset::OPEN));
    EXPECT_EQ(0u, manager_.closeableQty(0, OrderOffset::INVALID));
}

// CLOSE_TODAY reports exactly the position opened today (2 of the 6 total).
TEST_F(RiskManagerCloseableQuantityTest, CloseTodayReportsTheTodayQuantity) {
    seed();

    EXPECT_EQ(2u, manager_.closeableQty(0, OrderOffset::CLOSE_TODAY));
}

// CLOSE_YESTERDAY reports exactly the position carried from yesterday (4 of
// the 6 total).
TEST_F(RiskManagerCloseableQuantityTest, CloseYesterdayReportsTheYesterdayQuantity) {
    seed();

    EXPECT_EQ(4u, manager_.closeableQty(0, OrderOffset::CLOSE_YESTERDAY));
}

// The generic CLOSE offset reports the full position: today plus yesterday.
TEST_F(RiskManagerCloseableQuantityTest, CloseReportsBothDaysSummed) {
    seed();

    EXPECT_EQ(6u, manager_.closeableQty(0, OrderOffset::CLOSE));
}

}  // namespace