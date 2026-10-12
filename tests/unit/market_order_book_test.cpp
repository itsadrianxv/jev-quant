#include <gtest/gtest.h>

#include <stdexcept>

#include "exchange/market_data/market_update.h"
#include "trading/strategy/market_order_book.h"

namespace {

using Common::OrderId;
using Common::Price;
using Common::Qty;
using Common::Side;
using Exchange::MarketUpdate;
using Exchange::MarketUpdateType;
using Trading::MarketOrderBook;

MarketUpdate makeUpdate(MarketUpdateType type, OrderId order_id, Side side,
                        Price price, Qty qty) {
    MarketUpdate update;
    update.type_ = type;
    update.order_id_ = order_id;
    update.side_ = side;
    update.price_ = price;
    update.qty_ = qty;
    return update;
}

void expectBbo(const MarketOrderBook& book, Price bid_price, Qty bid_qty,
               Price ask_price, Qty ask_qty) {
    const Trading::BBO* bbo = book.getBBO();
    EXPECT_EQ(bid_price, bbo->bid_price_);
    EXPECT_EQ(bid_qty, bbo->bid_qty_);
    EXPECT_EQ(ask_price, bbo->ask_price_);
    EXPECT_EQ(ask_qty, bbo->ask_qty_);
}

TEST(MarketOrderBookTest, FreshBookReportsNoTopOfBook) {
    MarketOrderBook book(0);

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID,
              Common::Price_INVALID, Common::Qty_INVALID);
    EXPECT_EQ(MarketOrderBook::BookMode::UNINITIALIZED, book.mode());
}

TEST(MarketOrderBookTest, SnapshotUpdateDerivesBboFromFrontLevels) {
    MarketOrderBook book(0);
    MarketUpdate snapshot;
    snapshot.type_ = MarketUpdateType::DEPTH_SNAPSHOT;
    snapshot.depth_snapshot_.bids_[0] = {1233, 10};
    snapshot.depth_snapshot_.asks_[0] = {1235, 12};

    book.onMarketUpdate(snapshot);

    expectBbo(book, 1233, 10, 1235, 12);
    EXPECT_EQ(MarketOrderBook::BookMode::DEPTH_SNAPSHOT, book.mode());
}

TEST(MarketOrderBookTest, DirectSnapshotInstallInIncrementalModeThrowsLogicError) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));

    EXPECT_THROW(book.onDepthSnapshot(Exchange::DepthSnapshot{}), std::logic_error);
}

TEST(MarketOrderBookTest, IncrementalUpdateAfterSnapshotInstallThrowsLogicError) {
    MarketOrderBook book(0);
    MarketUpdate snapshot;
    snapshot.type_ = MarketUpdateType::DEPTH_SNAPSHOT;
    snapshot.depth_snapshot_.bids_[0] = {100, 1};
    snapshot.depth_snapshot_.asks_[0] = {101, 1};
    book.onMarketUpdate(snapshot);

    EXPECT_THROW(book.onMarketUpdate(
                         makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 99, 1)),
                 std::logic_error);
}

TEST(MarketOrderBookTest, AddsAggregateQuantitiesAtTheBestPrices) {
    MarketOrderBook book(0);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 3));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 3, Side::BUY, 99, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 5, Side::SELL, 103, 1));

    expectBbo(book, 100, 5, 102, 4);
}

TEST(MarketOrderBookTest, ModifyWithinLevelAdjustsAggregatedQuantity) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 3));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 2, Side::BUY, 100, 6));

    expectBbo(book, 100, 8, Common::Price_INVALID, Common::Qty_INVALID);
}

TEST(MarketOrderBookTest, ModifyAcrossPricesMovesTheRestingQuantity) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 99, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 2));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 1, Side::BUY, 100, 1));

    expectBbo(book, 100, 3, Common::Price_INVALID, Common::Qty_INVALID);
}

TEST(MarketOrderBookTest, ModifyAcrossPricesMovesTheBestAsk) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 5, Side::SELL, 103, 1));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 5, Side::SELL, 101, 1));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 101, 1);
}

TEST(MarketOrderBookTest, CancelsDrainTheLevelUntilItDisappears) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 3));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 3, Side::BUY, 99, 1));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 1, Side::INVALID, 0, 0));
    expectBbo(book, 100, 3, Common::Price_INVALID, Common::Qty_INVALID);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 2, Side::INVALID, 0, 0));
    expectBbo(book, 99, 1, Common::Price_INVALID, Common::Qty_INVALID);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 3, Side::INVALID, 0, 0));
    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID,
              Common::Price_INVALID, Common::Qty_INVALID);
}

TEST(MarketOrderBookTest, ReAddWithLiveOrderIdReplacesTheRestingOrder) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 104, 2));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 104, 2);
}

TEST(MarketOrderBookTest, ModifyOfUnknownOrderIdLeavesTheBookUnchanged) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 77, Side::SELL, 99, 5));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 102, 4);
}

TEST(MarketOrderBookTest, CancelOfUnknownOrderIdLeavesTheBookUnchanged) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 77, Side::INVALID, 0, 0));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 102, 4);
}

TEST(MarketOrderBookTest, SnapshotBoundaryUpdatesDoNotDisturbTheIncrementalBook) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::SNAPSHOT_START, 0, Side::INVALID, 0, 0));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::SNAPSHOT_END, 0, Side::INVALID, 0, 0));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 102, 4);
}

TEST(MarketOrderBookTest, TradeUpdatesAreConsumedWithoutChangingTheBook) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::TRADE, 9, Side::INVALID, 0, 0));

    expectBbo(book, 100, 2, 102, 4);
}

TEST(MarketOrderBookTest, ClearEmptiesTheBookAndAcceptsNewOrdersAfterwards) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 3));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 3, Side::BUY, 99, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 5, Side::SELL, 103, 1));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CLEAR, 0, Side::INVALID, 0, 0));
    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID,
              Common::Price_INVALID, Common::Qty_INVALID);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 3));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 3, Side::BUY, 99, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 5, Side::SELL, 103, 1));
    expectBbo(book, 100, 5, 102, 4);
}

TEST(MarketOrderBookTest, OrderSlotExhaustionFailsFastAtTheLoadFactorCeiling) {
    MarketOrderBook book(0);
    for (OrderId order_id = 1; order_id <= Trading::kMaxLiveOrderEntries; ++order_id) {
        book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, order_id, Side::BUY, 100, 1));
    }

    EXPECT_THROW(book.onMarketUpdate(makeUpdate(
                                          MarketUpdateType::ADD,
                                          Trading::kMaxLiveOrderEntries + 1,
                                          Side::BUY, 100, 1)),
                 std::runtime_error);
    EXPECT_EQ(Trading::kMaxLiveOrderEntries, book.getBBO()->bid_qty_);
}

TEST(MarketOrderBookTest, ClearedOrderSlotsAcceptNewOrdersAgain) {
    MarketOrderBook book(0);
    for (OrderId order_id = 1; order_id <= Trading::kMaxLiveOrderEntries; ++order_id) {
        book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, order_id, Side::BUY, 100, 1));
    }
    book.onMarketUpdate(makeUpdate(MarketUpdateType::CLEAR, 0, Side::INVALID, 0, 0));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 1));

    expectBbo(book, 100, 1, Common::Price_INVALID, Common::Qty_INVALID);
}

TEST(MarketOrderBookTest, LevelSlotExhaustionFailsFastPerSide) {
    MarketOrderBook book(0);
    for (Price price = 1;
         price <= static_cast<Price>(Trading::kMaxLiveLevelEntriesPerSide); ++price) {
        book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD,
                                       static_cast<OrderId>(price), Side::BUY,
                                       price, 1));
    }

    EXPECT_THROW(book.onMarketUpdate(makeUpdate(
                                          MarketUpdateType::ADD,
                                          static_cast<OrderId>(
                                                  Trading::kMaxLiveLevelEntriesPerSide + 1),
                                          Side::BUY,
                                          static_cast<Price>(
                                                  Trading::kMaxLiveLevelEntriesPerSide + 1),
                                          1)),
                 std::runtime_error);
    EXPECT_EQ(static_cast<Price>(Trading::kMaxLiveLevelEntriesPerSide),
              book.getBBO()->bid_price_);
    EXPECT_EQ(1, book.getBBO()->bid_qty_);
}

TEST(MarketOrderBookTest, ExhaustedBidSideLeavesTheAskSideUsable) {
    MarketOrderBook book(0);
    for (Price price = 1;
         price <= static_cast<Price>(Trading::kMaxLiveLevelEntriesPerSide); ++price) {
        book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD,
                                       static_cast<OrderId>(price), Side::BUY,
                                       price, 1));
    }

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1000000, Side::SELL,
                                   1000000, 1));

    EXPECT_EQ(1000000, book.getBBO()->ask_price_);
}

TEST(MarketOrderBookTest, FreedLevelSlotsAcceptNewPricesAgain) {
    MarketOrderBook book(0);
    for (Price price = 1;
         price <= static_cast<Price>(Trading::kMaxLiveLevelEntriesPerSide); ++price) {
        book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD,
                                       static_cast<OrderId>(price), Side::BUY,
                                       price, 1));
    }
    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 1, Side::INVALID, 0, 0));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2000000, Side::BUY,
                                   2000000, 1));

    EXPECT_EQ(2000000, book.getBBO()->bid_price_);
}

TEST(MarketOrderBookTest, ModifyToANewLevelAtExhaustionFailsFastAndKeepsTheRestingOrder) {
    MarketOrderBook book(0);
    for (Price price = 1;
         price <= static_cast<Price>(Trading::kMaxLiveLevelEntriesPerSide); ++price) {
        book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD,
                                       static_cast<OrderId>(price), Side::BUY,
                                       price, 1));
    }

    EXPECT_THROW(book.onMarketUpdate(makeUpdate(
                                          MarketUpdateType::MODIFY, 1, Side::BUY,
                                          9000000, 1)),
                 std::runtime_error);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 1, Side::INVALID, 0, 0));
    EXPECT_EQ(static_cast<Price>(Trading::kMaxLiveLevelEntriesPerSide),
              book.getBBO()->bid_price_);
}

TEST(MarketOrderBookTest, ErasingAMiddleCollidingOrderKeepsProbeChainOrdersReachable) {
    // Order ids 1, 65537, 131073 all hash to the same order-slot home, so
    // erasing the middle one exercises the backward-shift compaction.
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 65537, Side::BUY, 100, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 131073, Side::BUY, 100, 1));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 65537, Side::INVALID, 0, 0));
    expectBbo(book, 100, 2, Common::Price_INVALID, Common::Qty_INVALID);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 65537, Side::BUY, 100, 1));
    expectBbo(book, 100, 3, Common::Price_INVALID, Common::Qty_INVALID);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 65537, Side::INVALID, 0, 0));
    expectBbo(book, 100, 2, Common::Price_INVALID, Common::Qty_INVALID);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 131073, Side::INVALID, 0, 0));
    expectBbo(book, 100, 1, Common::Price_INVALID, Common::Qty_INVALID);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 1, Side::INVALID, 0, 0));
    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID,
              Common::Price_INVALID, Common::Qty_INVALID);
}

TEST(MarketOrderBookTest, ErasingAMiddleCollidingLevelKeepsProbeChainLevelsReachable) {
    // Prices 1, 4097, 8193 all hash to the same level-slot home on the bid
    // side, so erasing the middle level exercises the backward-shift
    // compaction of the level slot array.
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 1, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 4097, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 3, Side::BUY, 8193, 1));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 2, Side::INVALID, 0, 0));
    EXPECT_EQ(8193, book.getBBO()->bid_price_);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 3, Side::INVALID, 0, 0));
    EXPECT_EQ(1, book.getBBO()->bid_price_);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::BUY, 4097, 1));
    EXPECT_EQ(4097, book.getBBO()->bid_price_);
}

}  // namespace
