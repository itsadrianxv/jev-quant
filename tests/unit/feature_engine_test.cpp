#include <gtest/gtest.h>

#include "common/types.h"
#include "trading/strategy/feature_engine.h"
#include "trading/strategy/market_order_book.h"

namespace {

using Common::Price;
using Common::Qty;

void installDepthSnapshot(Trading::MarketOrderBook& book, Price bid, Price ask,
                          Qty bid_qty, Qty ask_qty) {
    Exchange::DepthSnapshot depth;
    depth.bids_[0] = {bid, bid_qty};
    depth.asks_[0] = {ask, ask_qty};
    book.onDepthSnapshot(depth);
}

struct InvalidTopOfBookRow {
    const char* label;
    Price bid;
    Price ask;
    Qty bid_qty;
    Qty ask_qty;
};

constexpr InvalidTopOfBookRow kInvalidTopOfBookRows[] = {
        {"CrossedMarket", 101, 100, 1, 1},
        {"LockedMarket", 100, 100, 1, 1},
        {"ZeroBidPrice", 0, 100, 1, 1},
        {"NegativeBidPrice", -1, 100, 1, 1},
        {"SentinelBidPrice", Common::Price_INVALID, 101, 1, 1},
        {"SentinelAskPrice", 100, Common::Price_INVALID, 1, 1},
        {"ZeroBidQuantity", 100, 101, 0, 1},
        {"ZeroAskQuantity", 100, 101, 1, 0},
        {"SentinelBidQuantity", 100, 101, Common::Qty_INVALID, 1},
        {"SentinelAskQuantity", 100, 101, 1, Common::Qty_INVALID},
};

class FeatureEngineTest : public ::testing::Test {
  protected:
    void publishTopOfBook(Price bid, Price ask, Qty bid_qty, Qty ask_qty) {
        installDepthSnapshot(book_, bid, ask, bid_qty, ask_qty);
        engine_.onMarketUpdate(book_);
    }

    Trading::MarketOrderBook book_{0};
    Trading::FeatureEngine engine_;
};

class FeatureEngineInvalidTopOfBookTest : public FeatureEngineTest,
                                          public ::testing::WithParamInterface<
                                                  InvalidTopOfBookRow> {};

// Table-driven: each crossed, locked, or sentinel-laden top of book must
// reset every feature to invalid/zero instead of computing garbage from
// unusable quotes.
TEST_P(FeatureEngineInvalidTopOfBookTest, ResetsTheFeaturesToInvalid) {
    const auto& row = GetParam();
    publishTopOfBook(row.bid, row.ask, row.bid_qty, row.ask_qty);

    const auto& features = engine_.features();
    EXPECT_FALSE(features.valid_);
    EXPECT_DOUBLE_EQ(0.0, features.mid_price_);
    EXPECT_DOUBLE_EQ(0.0, features.relative_spread_bps_);
    EXPECT_EQ(0u, features.top_of_book_total_qty_);
    EXPECT_DOUBLE_EQ(0.0, features.top_of_book_qty_imbalance_);
}

INSTANTIATE_TEST_SUITE_P(TruthTable, FeatureEngineInvalidTopOfBookTest,
                         ::testing::ValuesIn(kInvalidTopOfBookRows),
                         [](const ::testing::TestParamInfo<InvalidTopOfBookRow>&
                                    info) { return info.param.label; });

// A freshly constructed engine has not seen any quote yet and must report
// invalid (all-zero) features.
TEST_F(FeatureEngineTest, FreshEngineReportsInvalidFeatures) {
    EXPECT_FALSE(engine_.features().valid_);
    EXPECT_DOUBLE_EQ(0.0, engine_.features().mid_price_);
}

// A valid top of book (bid 100/2, ask 101/6) yields mid 100.5, relative
// spread in bps, total top qty 8, and imbalance -0.5 (ask-heavy).
TEST_F(FeatureEngineTest, ValidTopOfBookComputesMidSpreadQtyAndImbalance) {
    publishTopOfBook(100, 101, 2, 6);

    const auto& features = engine_.features();
    EXPECT_TRUE(features.valid_);
    EXPECT_DOUBLE_EQ(100.5, features.mid_price_);
    EXPECT_NEAR(features.relative_spread_bps_, 99.502487562189055, 1e-9);
    EXPECT_EQ(8u, features.top_of_book_total_qty_);
    EXPECT_DOUBLE_EQ(-0.5, features.top_of_book_qty_imbalance_);
}

// Mirroring the quantities (bid 6, ask 2) flips the imbalance sign to +0.5
// while everything else stays consistent.
TEST_F(FeatureEngineTest, BidHeavyTopOfBookMakesTheImbalancePositive) {
    publishTopOfBook(100, 101, 6, 2);

    const auto& features = engine_.features();
    EXPECT_TRUE(features.valid_);
    EXPECT_EQ(8u, features.top_of_book_total_qty_);
    EXPECT_DOUBLE_EQ(0.5, features.top_of_book_qty_imbalance_);
}

// Quotes one step below the sentinel values are still legitimate numbers:
// they must be treated as valid and produce huge-but-correct feature values,
// proving the engine does not treat near-sentinel data as invalid.
TEST_F(FeatureEngineTest, NearSentinelQuantitiesStayValid) {
    const Price bid = Common::Price_INVALID - 100;
    const Price ask = Common::Price_INVALID - 1;
    const Qty qty = Common::Qty_INVALID - 1;

    publishTopOfBook(bid, ask, qty, qty);

    const auto& features = engine_.features();
    EXPECT_TRUE(features.valid_);
    EXPECT_EQ(2ULL * (Common::Qty_INVALID - 1), features.top_of_book_total_qty_);
    EXPECT_GT(features.relative_spread_bps_, 0.0);
}

// After an invalid top of book resets the features, the next valid top of
// book must restore valid features (recovery works).
TEST_F(FeatureEngineTest, InvalidTopOfBookStaysResetUntilAValidUpdate) {
    publishTopOfBook(101, 100, 1, 1);
    EXPECT_FALSE(engine_.features().valid_);

    publishTopOfBook(99, 101, 1, 1);

    const auto& features = engine_.features();
    EXPECT_TRUE(features.valid_);
    EXPECT_DOUBLE_EQ(100.0, features.mid_price_);
}

}  // namespace
