#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <stdexcept>
#include <unordered_map>

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

// A fresh book has no quotes: BBO carries sentinel prices/quantities and the
// mode is still UNINITIALIZED until a snapshot or incremental update arrives.
TEST(MarketOrderBookTest, FreshBookReportsNoTopOfBook) {
    MarketOrderBook book(0);

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID,
              Common::Price_INVALID, Common::Qty_INVALID);
    EXPECT_EQ(MarketOrderBook::BookMode::UNINITIALIZED, book.mode());
}

// A DEPTH_SNAPSHOT update switches the book into snapshot mode and derives
// the BBO from the front bid/ask levels of the snapshot.
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

TEST(MarketOrderBookTest, SnapshotReplacementPreservesDeepLevelsAndClearsOldMetadata) {
    MarketOrderBook book(0);
    Exchange::DepthSnapshot full;
    for (std::size_t i = 0; i < Common::DEPTH_SNAPSHOT_LEVELS; ++i) {
        full.bids_[i] = {100 - static_cast<Price>(i), static_cast<Qty>(i + 1)};
        full.asks_[i] = {101 + static_cast<Price>(i), static_cast<Qty>(i + 2)};
    }
    full.last_price_ = 101;
    full.volume_ = 123;
    full.open_interest_ = 456.5;
    full.upper_limit_price_ = 120;
    full.lower_limit_price_ = 80;
    book.onDepthSnapshot(full);
    const auto* actual = book.getDepthSnapshot();
    for (std::size_t i = 0; i < Common::DEPTH_SNAPSHOT_LEVELS; ++i) {
        EXPECT_EQ(actual->bids_[i].price_, full.bids_[i].price_);
        EXPECT_EQ(actual->bids_[i].qty_, full.bids_[i].qty_);
        EXPECT_EQ(actual->asks_[i].price_, full.asks_[i].price_);
        EXPECT_EQ(actual->asks_[i].qty_, full.asks_[i].qty_);
    }
    EXPECT_EQ(actual->last_price_, 101);
    EXPECT_EQ(actual->volume_, 123U);
    EXPECT_EQ(actual->open_interest_, 456.5);
    EXPECT_EQ(actual->upper_limit_price_, 120);
    EXPECT_EQ(actual->lower_limit_price_, 80);
    Exchange::DepthSnapshot replacement;
    replacement.bids_[0] = {98, 7};
    book.onDepthSnapshot(replacement);
    actual = book.getDepthSnapshot();
    EXPECT_EQ(actual->bids_[0].price_, 98);
    for (std::size_t i = 1; i < Common::DEPTH_SNAPSHOT_LEVELS; ++i) {
        EXPECT_EQ(actual->bids_[i].price_, Common::Price_INVALID);
        EXPECT_EQ(actual->bids_[i].qty_, Common::Qty_INVALID);
    }
    for (const auto& ask : actual->asks_) {
        EXPECT_EQ(ask.price_, Common::Price_INVALID);
        EXPECT_EQ(ask.qty_, Common::Qty_INVALID);
    }
    EXPECT_EQ(actual->last_price_, Common::Price_INVALID);
    EXPECT_EQ(actual->volume_, Common::Qty_INVALID);
    EXPECT_EQ(actual->open_interest_, 0.0);
    EXPECT_EQ(actual->upper_limit_price_, Common::Price_INVALID);
    EXPECT_EQ(actual->lower_limit_price_, Common::Price_INVALID);
}

// Installing a depth snapshot after the book has already received an
// incremental (ADD) update is a mode violation and must fail fast.
TEST(MarketOrderBookTest, DirectSnapshotInstallInIncrementalModeThrowsLogicError) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));

    EXPECT_THROW(book.onDepthSnapshot(Exchange::DepthSnapshot{}), std::logic_error);
}

// Feeding an incremental ADD after the book has switched to snapshot mode is
// a mode violation and must fail fast.
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

// Multiple ADDs at the same price aggregate into one level quantity: the
// best bid is 100 with 2+3=5 and the best ask is 102 with 4.
TEST(MarketOrderBookTest, AddsAggregateQuantitiesAtTheBestPrices) {
    MarketOrderBook book(0);

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 3));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 3, Side::BUY, 99, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 5, Side::SELL, 103, 1));

    expectBbo(book, 100, 5, 102, 4);
}

// A MODIFY that stays at the same price replaces the order's contribution to
// that level (order 2 goes 3 -> 6, level total 2+6=8).
TEST(MarketOrderBookTest, ModifyWithinLevelAdjustsAggregatedQuantity) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 3));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 2, Side::BUY, 100, 6));

    expectBbo(book, 100, 8, Common::Price_INVALID, Common::Qty_INVALID);
}

// A MODIFY to a different price moves the order between levels: 99/1 leaves
// and reappears at 100, so the best bid becomes 100 with 1+2=3.
TEST(MarketOrderBookTest, ModifyAcrossPricesMovesTheRestingQuantity) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 99, 1));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 2, Side::BUY, 100, 2));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 1, Side::BUY, 100, 1));

    expectBbo(book, 100, 3, Common::Price_INVALID, Common::Qty_INVALID);
}

// A MODIFY can improve the ask: moving order 5 from 103 down to 101 makes it
// the new best ask.
TEST(MarketOrderBookTest, ModifyAcrossPricesMovesTheBestAsk) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 5, Side::SELL, 103, 1));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 5, Side::SELL, 101, 1));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 101, 1);
}

// Cancels remove each order's contribution; when the last order at a level
// is cancelled the level disappears and the BBO falls back to the next best
// price, eventually to no bid at all.
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

// An ADD for an order id that is still live replaces the resting order:
// 102/4 is replaced by 104/2 rather than aggregated.
TEST(MarketOrderBookTest, ReAddWithLiveOrderIdReplacesTheRestingOrder) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 104, 2));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 104, 2);
}

// A MODIFY referencing an unknown order id is a no-op on the book.
TEST(MarketOrderBookTest, ModifyOfUnknownOrderIdLeavesTheBookUnchanged) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::MODIFY, 77, Side::SELL, 99, 5));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 102, 4);
}

// A CANCEL referencing an unknown order id is a no-op on the book.
TEST(MarketOrderBookTest, CancelOfUnknownOrderIdLeavesTheBookUnchanged) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::CANCEL, 77, Side::INVALID, 0, 0));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 102, 4);
}

// SNAPSHOT_START and SNAPSHOT_END boundary markers are consumed without
// touching the book contents, so the incremental BBO survives.
TEST(MarketOrderBookTest, SnapshotBoundaryUpdatesDoNotDisturbTheIncrementalBook) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::SNAPSHOT_START, 0, Side::INVALID, 0, 0));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::SNAPSHOT_END, 0, Side::INVALID, 0, 0));

    expectBbo(book, Common::Price_INVALID, Common::Qty_INVALID, 102, 4);
}

// TRADE updates carry no book mutation: the BBO stays exactly as it was.
TEST(MarketOrderBookTest, TradeUpdatesAreConsumedWithoutChangingTheBook) {
    MarketOrderBook book(0);
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 2));
    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 4, Side::SELL, 102, 4));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::TRADE, 9, Side::INVALID, 0, 0));

    expectBbo(book, 100, 2, 102, 4);
}

// CLEAR wipes every order and level, then the book accepts the same orders
// again as if it were fresh (ids can be reused after a clear).
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

// Filling every order slot up to the load-factor ceiling works, but the next
// ADD must fail fast with a runtime_error instead of silently corrupting the
// pooled order slots.
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

// After a CLEAR the exhausted order slots are all released, so new orders
// are accepted again.
TEST(MarketOrderBookTest, ClearedOrderSlotsAcceptNewOrdersAgain) {
    MarketOrderBook book(0);
    for (OrderId order_id = 1; order_id <= Trading::kMaxLiveOrderEntries; ++order_id) {
        book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, order_id, Side::BUY, 100, 1));
    }
    book.onMarketUpdate(makeUpdate(MarketUpdateType::CLEAR, 0, Side::INVALID, 0, 0));

    book.onMarketUpdate(makeUpdate(MarketUpdateType::ADD, 1, Side::BUY, 100, 1));

    expectBbo(book, 100, 1, Common::Price_INVALID, Common::Qty_INVALID);
}

// Filling every bid price level up to the per-side ceiling works, but adding
// one more level must fail fast with a runtime_error.
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

// Level slot exhaustion on the bid side does not affect the ask side: asks
// can still be added.
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

// Cancelling the order at a level frees its level slot, so a new price level
// is accepted again afterwards.
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

// A MODIFY that would need a new level slot while all slots are taken fails
// fast, and the original resting order stays intact (it can still be
// cancelled afterwards).
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

// Order ids 1, 65537, 131073 all hash to the same order-slot home, so
// erasing the middle one exercises the backward-shift compaction: the
// remaining probe chain orders must stay reachable and the freed slot
// reusable.
TEST(MarketOrderBookTest, ErasingAMiddleCollidingOrderKeepsProbeChainOrdersReachable) {
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

// Property test: replays a deterministic seeded sequence of 20000 random
// ADD/MODIFY/CANCEL/CLEAR/TRADE operations against both the pooled book and
// a simple ordered-map reference model, comparing the BBO after every
// operation so any divergence is caught at the exact step.
TEST(MarketOrderBookTest, MatchesTheReferenceModelUnderSeededRandomOperations) {
    // Mirror of the pre-pooled ordered-map book behavior, compared op by op
    // under a fixed seed so the sequence is fully deterministic.
    struct ModelOrder {
        Side side_ = Side::INVALID;
        Price price_ = Common::Price_INVALID;
        Qty qty_ = 0;
    };
    std::unordered_map<OrderId, ModelOrder> orders;
    std::map<Price, Qty, std::greater<Price>> bids;
    std::map<Price, Qty> asks;

    const auto add_level = [&](Side side, Price price, Qty qty) {
        if (side == Side::BUY) {
            bids[price] += qty;
        } else {
            asks[price] += qty;
        }
    };
    const auto remove_level = [&](Side side, Price price, Qty qty) {
        if (side == Side::BUY) {
            const auto level = bids.find(price);
            if (level == bids.end()) {
                return;
            }
            if (level->second <= qty) {
                bids.erase(level);
            } else {
                level->second -= qty;
            }
        } else {
            const auto level = asks.find(price);
            if (level == asks.end()) {
                return;
            }
            if (level->second <= qty) {
                asks.erase(level);
            } else {
                level->second -= qty;
            }
        }
    };
    const auto cancel_order = [&](OrderId order_id) {
        const auto order = orders.find(order_id);
        if (order == orders.end()) {
            return;
        }
        remove_level(order->second.side_, order->second.price_, order->second.qty_);
        orders.erase(order);
    };

    MarketOrderBook book(0);
    int operation = 0;

    const auto apply = [&](MarketUpdateType type, OrderId order_id, Side side,
                           Price price, Qty qty) {
        ++operation;
        MarketUpdate update;
        update.type_ = type;
        update.order_id_ = order_id;
        update.side_ = side;
        update.price_ = price;
        update.qty_ = qty;

        switch (type) {
            case MarketUpdateType::ADD:
                cancel_order(order_id);
                orders[order_id] = {side, price, qty};
                add_level(side, price, qty);
                break;
            case MarketUpdateType::MODIFY: {
                const auto order = orders.find(order_id);
                if (order == orders.end()) {
                    break;
                }
                auto& resting = order->second;
                if (resting.side_ != side || resting.price_ != price) {
                    remove_level(resting.side_, resting.price_, resting.qty_);
                    resting.side_ = side;
                    resting.price_ = price;
                    add_level(resting.side_, resting.price_, qty);
                } else if (qty >= resting.qty_) {
                    add_level(resting.side_, resting.price_, qty - resting.qty_);
                } else {
                    remove_level(resting.side_, resting.price_, resting.qty_ - qty);
                }
                resting.qty_ = qty;
                break;
            }
            case MarketUpdateType::CANCEL:
                cancel_order(order_id);
                break;
            case MarketUpdateType::CLEAR:
                orders.clear();
                bids.clear();
                asks.clear();
                break;
            default:
                break;
        }

        book.onMarketUpdate(update);

        const Trading::BBO* bbo = book.getBBO();
        const auto bid = bids.begin();
        const auto ask = asks.begin();
        EXPECT_EQ(bid == bids.end() ? Common::Price_INVALID : bid->first, bbo->bid_price_)
                << "bid price diverged at operation " << operation;
        EXPECT_EQ(bid == bids.end() ? Common::Qty_INVALID : bid->second, bbo->bid_qty_)
                << "bid qty diverged at operation " << operation;
        EXPECT_EQ(ask == asks.end() ? Common::Price_INVALID : ask->first, bbo->ask_price_)
                << "ask price diverged at operation " << operation;
        EXPECT_EQ(ask == asks.end() ? Common::Qty_INVALID : ask->second, bbo->ask_qty_)
                << "ask qty diverged at operation " << operation;
    };

    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto next = [&state]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 33;
    };
    for (int i = 0; i < 20000; ++i) {
        const auto roll = next() % 100;
        const auto order_id = static_cast<OrderId>(next() % 64);
        const auto side = (next() % 2) == 0 ? Side::BUY : Side::SELL;
        const auto price = static_cast<Price>(90 + next() % 20);
        const auto qty = static_cast<Qty>(1 + next() % 5);
        if (roll < 3) {
            apply(MarketUpdateType::TRADE, order_id, side, price, qty);
        } else if (roll < 48) {
            apply(MarketUpdateType::ADD, order_id, side, price, qty);
        } else if (roll < 78) {
            apply(MarketUpdateType::MODIFY, order_id, side, price, qty);
        } else if (roll < 96) {
            apply(MarketUpdateType::CANCEL, order_id, side, price, qty);
        } else {
            apply(MarketUpdateType::CLEAR, 0, Side::INVALID, 0, 0);
        }
    }
}

// Prices 1, 4097, 8193 all hash to the same level-slot home on the bid
// side, so erasing the middle level exercises the backward-shift
// compaction of the level slot array: remaining levels stay reachable as
// best bid and the freed slot accepts a re-added price again.
TEST(MarketOrderBookTest, ErasingAMiddleCollidingLevelKeepsProbeChainLevelsReachable) {
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
