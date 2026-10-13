#include <gtest/gtest.h>

#include <cstdio>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "exchange/market_data/market_update.h"
#include "trading/venue/binance_depth_book.h"

namespace {

using Common::TickerId;
using Exchange::DepthSnapshot;
using Exchange::MarketUpdate;
using Exchange::MarketUpdateType;
using Trading::BinanceDepthBook;

nlohmann::json priceLevel(const char* price, const char* qty) {
    return nlohmann::json::array({price, qty});
}

nlohmann::json priceLevels(std::initializer_list<nlohmann::json> levels) {
    return nlohmann::json(levels);
}

nlohmann::json depthEvent(std::uint64_t first, std::uint64_t final_update,
                          std::uint64_t previous, const nlohmann::json& bids,
                          const nlohmann::json& asks) {
    return nlohmann::json{{"U", first}, {"u", final_update}, {"pu", previous},
                          {"b", bids}, {"a", asks}};
}

std::string gridPrice(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    return buffer;
}

// A freshly constructed book has not seen a snapshot: not ready, no best
// bid/ask, and no buffered pending events.
TEST(BinanceDepthBookTest, FreshBookIsNotReadyAndHasNoTopOfBook) {
    BinanceDepthBook book;

    EXPECT_FALSE(book.snapshotReady());
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_EQ(0u, book.pendingCount());
}

// Depth events arriving before a snapshot cannot be applied yet; they are
// buffered in the pending ring instead of mutating an unprimed book.
TEST(BinanceDepthBookTest, EventsBeforeSnapshotBufferInThePendingRing) {
    BinanceDepthBook book;

    book.onDepthEvent(depthEvent(10, 12, 9,
                                 priceLevels({priceLevel("100.5", "2")}),
                                 priceLevels({priceLevel("101.0", "3")})));

    EXPECT_FALSE(book.snapshotReady());
    EXPECT_EQ(1u, book.pendingCount());
    EXPECT_FALSE(book.bestBid().has_value());
}

// Installing a snapshot primes the book and then replays all buffered
// pending events in order, ending up with the post-event state (snapshot
// level plus the buffered update).
TEST(BinanceDepthBookTest, SnapshotInstallAppliesTheBufferedEventsInOrder) {
    BinanceDepthBook book;
    book.onDepthEvent(depthEvent(10, 12, 9,
                                 priceLevels({priceLevel("100.5", "2")}),
                                 priceLevels({priceLevel("101.0", "3")})));

    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 9},
            {"bids", priceLevels({priceLevel("100.0", "1")})},
            {"asks", priceLevels({priceLevel("100.9", "5")})}});

    EXPECT_TRUE(book.snapshotReady());
    EXPECT_EQ(0u, book.pendingCount());
    EXPECT_EQ(12u, book.lastUpdateId());
    EXPECT_EQ(2u, book.bidCount());
    EXPECT_EQ(2u, book.askCount());
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(100.5, book.bestBid()->price);
    EXPECT_EQ(2.0, book.bestBid()->qty);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(100.9, book.bestAsk()->price);
}

// An event whose final update id is not newer than the installed snapshot's
// lastUpdateId is stale and must be ignored entirely.
TEST(BinanceDepthBookTest, StaleEventsAreSkippedWithoutChangingTheBook) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 12},
            {"bids", priceLevels({priceLevel("100.0", "1"), priceLevel("100.5", "2")})},
            {"asks", priceLevels({priceLevel("100.9", "5")})}});

    book.onDepthEvent(depthEvent(5, 6, 4,
                                 priceLevels({priceLevel("100.5", "9")}),
                                 priceLevels({})));

    EXPECT_EQ(2u, book.bidCount());
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(2.0, book.bestBid()->qty);
    EXPECT_EQ(12u, book.lastUpdateId());
}

// An event with a non-zero quantity at an existing price updates that
// level's quantity in place without adding a new level.
TEST(BinanceDepthBookTest, NonZeroQuantityUpdatesTheLevelInPlace) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 12},
            {"bids", priceLevels({priceLevel("100.0", "1"), priceLevel("100.5", "2")})},
            {"asks", priceLevels({priceLevel("100.9", "5")})}});

    book.onDepthEvent(depthEvent(13, 14, 12,
                                 priceLevels({priceLevel("100.5", "7")}),
                                 priceLevels({})));

    EXPECT_EQ(2u, book.bidCount());
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(7.0, book.bestBid()->qty);
}

// Per Binance semantics a zero quantity in an event deletes the price level
// on both sides.
TEST(BinanceDepthBookTest, ZeroQuantityRemovesTheLevel) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 15},
            {"bids", priceLevels({priceLevel("100.0", "1"), priceLevel("100.5", "2")})},
            {"asks", priceLevels({priceLevel("100.9", "5"), priceLevel("101.0", "3")})}});

    book.onDepthEvent(depthEvent(16, 17, 15,
                                 priceLevels({priceLevel("100.5", "0")}),
                                 priceLevels({priceLevel("100.9", "0")})));

    EXPECT_EQ(1u, book.bidCount());
    EXPECT_EQ(1u, book.askCount());
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(100.0, book.bestBid()->price);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(101.0, book.bestAsk()->price);
}

// An event missing required fields (U/u/pu/b/a) is malformed and must fail
// fast with a runtime error.
TEST(BinanceDepthBookTest, MalformedEventThrowsRuntimeError) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 100},
            {"bids", priceLevels({priceLevel("100.0", "1")})},
            {"asks", priceLevels({priceLevel("101.0", "1")})}});

    EXPECT_THROW(book.onDepthEvent(nlohmann::json{{"U", 1}}), std::runtime_error);
}

// A snapshot whose levels arrive in arbitrary order is normalized on
// install: bids sorted descending, asks ascending, so level 0 is always the
// best price.
TEST(BinanceDepthBookTest, SnapshotInstallSortsBidsDescendingAndAsksAscending) {
    BinanceDepthBook book;

    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 7},
            {"bids", priceLevels({priceLevel("100.0", "1"), priceLevel("99.0", "1")})},
            {"asks", priceLevels({priceLevel("101.0", "1")})}});

    EXPECT_TRUE(book.snapshotReady());
    EXPECT_EQ(7u, book.lastUpdateId());
    EXPECT_EQ(2u, book.bidCount());
    EXPECT_EQ(1u, book.askCount());
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(100.0, book.bestBid()->price);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(101.0, book.bestAsk()->price);
}

// Reinstalling a snapshot replaces the whole book contents (no stale levels
// carried over) and advances lastUpdateId.
TEST(BinanceDepthBookTest, ReinstallSnapshotReplacesEveryLevel) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 7},
            {"bids", priceLevels({priceLevel("100.0", "1"), priceLevel("99.0", "1")})},
            {"asks", priceLevels({priceLevel("101.0", "1")})}});

    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 20},
            {"bids", priceLevels({priceLevel("98.0", "3")})},
            {"asks", priceLevels({priceLevel("103.0", "5"), priceLevel("102.0", "4")})}});

    EXPECT_EQ(1u, book.bidCount());
    EXPECT_EQ(2u, book.askCount());
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(98.0, book.bestBid()->price);
    ASSERT_TRUE(book.bestAsk().has_value());
    EXPECT_EQ(102.0, book.bestAsk()->price);
    EXPECT_EQ(20u, book.lastUpdateId());
}

// buildMarketUpdate emits a DEPTH_SNAPSHOT for the given ticker with double
// prices scaled into integer Price ticks, bids and asks each in best-first
// order.
TEST(BinanceDepthBookTest, PublishedMarketUpdateScalesPricesInLevelOrder) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 20},
            {"bids", priceLevels({priceLevel("98.0", "3")})},
            {"asks", priceLevels({priceLevel("103.0", "5"), priceLevel("102.0", "4")})}});

    const MarketUpdate published = book.buildMarketUpdate(3);

    EXPECT_EQ(MarketUpdateType::DEPTH_SNAPSHOT, published.type_);
    EXPECT_EQ(3u, published.ticker_id_);
    EXPECT_EQ(static_cast<Common::Price>(98.0 * 100000000.0),
              published.depth_snapshot_.bids_[0].price_);
    EXPECT_EQ(static_cast<Common::Price>(102.0 * 100000000.0),
              published.depth_snapshot_.asks_[0].price_);
    EXPECT_EQ(static_cast<Common::Price>(103.0 * 100000000.0),
              published.depth_snapshot_.asks_[1].price_);
}

// An event that skips ahead of lastUpdateId+1 without covering the gap is a
// sequence break: it must throw and leave the book state untouched.
TEST(BinanceDepthBookTest, SequenceGapThrowsAndLeavesTheBookUnchanged) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 100},
            {"bids", priceLevels({priceLevel("100.0", "1")})},
            {"asks", priceLevels({priceLevel("101.0", "1")})}});

    EXPECT_THROW(book.onDepthEvent(depthEvent(200, 210, 199, priceLevels({}),
                                              priceLevels({}))),
                 std::runtime_error);
    EXPECT_EQ(100u, book.lastUpdateId());
    EXPECT_EQ(1u, book.bidCount());
}

// An event whose [U,u] range overlaps already-applied ids is tolerated as
// long as it reaches lastUpdateId+1; it is applied and advances the id.
TEST(BinanceDepthBookTest, OverlappingUpdateRangeIsTolerated) {
    BinanceDepthBook book;
    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 100},
            {"bids", priceLevels({priceLevel("100.0", "1")})},
            {"asks", priceLevels({priceLevel("101.0", "1")})}});

    book.onDepthEvent(depthEvent(50, 150, 999,
                                 priceLevels({priceLevel("99.5", "2")}),
                                 priceLevels({})));

    EXPECT_EQ(150u, book.lastUpdateId());
    EXPECT_EQ(2u, book.bidCount());
}

// The pending ring holds at most kPendingEventCapacity pre-snapshot events;
// a snapshot install drains it and replays every buffered event in order.
TEST(BinanceDepthBookTest, PendingRingIsBoundedAtTheEventCapacity) {
    BinanceDepthBook book;
    for (std::size_t i = 0; i < BinanceDepthBook::kPendingEventCapacity; ++i) {
        const auto first = 2 + static_cast<std::uint64_t>(2 * i);
        book.onDepthEvent(depthEvent(
                first, first + 1, first - 1,
                priceLevels({priceLevel(gridPrice(50.0 + 0.1 * (i + 1)).c_str(), "1")}),
                priceLevels({})));
    }
    EXPECT_EQ(BinanceDepthBook::kPendingEventCapacity, book.pendingCount());

    book.installSnapshot(nlohmann::json{
            {"lastUpdateId", 1},
            {"bids", priceLevels({priceLevel("50.0", "1")})},
            {"asks", priceLevels({priceLevel("200.0", "1")})}});

    EXPECT_EQ(0u, book.pendingCount());
    EXPECT_EQ(1u + BinanceDepthBook::kPendingEventCapacity, book.bidCount());
    EXPECT_EQ(1u + 2 * BinanceDepthBook::kPendingEventCapacity, book.lastUpdateId());
}

// Mirrors the previous ordered-map depth book so the pooled book can be
// checked for publish equivalence against the retired implementation.
struct DepthBookReference {
    std::map<double, double> bids;
    std::map<double, double> asks;
    std::uint64_t last = 0;

    auto apply(const nlohmann::json& message) -> void {
        if (!message.contains("U") || !message.contains("u") ||
            !message.contains("pu") || !message.contains("b") ||
            !message.contains("a")) {
            throw std::runtime_error("Invalid Binance depth event");
        }
        const auto first = message.at("U").get<std::uint64_t>();
        const auto final_update = message.at("u").get<std::uint64_t>();
        const auto previous = message.at("pu").get<std::uint64_t>();
        if (final_update <= last) return;
        if (last != 0 && previous != last &&
            !(first <= last + 1 && final_update >= last + 1)) {
            throw std::runtime_error("Binance depth sequence gap");
        }
        for (const auto& level : message.at("b")) {
            const auto price = std::stod(level[0].get<std::string>());
            const auto quantity = std::stod(level[1].get<std::string>());
            if (quantity == 0.0) bids.erase(price); else bids[price] = quantity;
        }
        for (const auto& level : message.at("a")) {
            const auto price = std::stod(level[0].get<std::string>());
            const auto quantity = std::stod(level[1].get<std::string>());
            if (quantity == 0.0) asks.erase(price); else asks[price] = quantity;
        }
        last = final_update;
    }

    auto install(const nlohmann::json& depth) -> void {
        bids.clear();
        asks.clear();
        for (const auto& level : depth.at("bids")) {
            bids[std::stod(level[0].get<std::string>())] =
                    std::stod(level[1].get<std::string>());
        }
        for (const auto& level : depth.at("asks")) {
            asks[std::stod(level[0].get<std::string>())] =
                    std::stod(level[1].get<std::string>());
        }
        last = depth.at("lastUpdateId").get<std::uint64_t>();
    }

    auto publish(TickerId ticker_id) const -> MarketUpdate {
        MarketUpdate update;
        update.type_ = MarketUpdateType::DEPTH_SNAPSHOT;
        update.ticker_id_ = ticker_id;
        std::size_t index = 0;
        for (auto it = bids.rbegin();
             it != bids.rend() && index < Common::DEPTH_SNAPSHOT_LEVELS;
             ++it, ++index) {
            update.depth_snapshot_.bids_[index].price_ =
                    static_cast<Common::Price>(it->first * 100000000.0);
            update.depth_snapshot_.bids_[index].qty_ =
                    static_cast<Common::Qty>(it->second);
        }
        index = 0;
        for (auto it = asks.begin();
             it != asks.end() && index < Common::DEPTH_SNAPSHOT_LEVELS;
             ++it, ++index) {
            update.depth_snapshot_.asks_[index].price_ =
                    static_cast<Common::Price>(it->first * 100000000.0);
            update.depth_snapshot_.asks_[index].qty_ =
                    static_cast<Common::Qty>(it->second);
        }
        return update;
    }
};

auto sameDepthSnapshot(const DepthSnapshot& left, const DepthSnapshot& right)
        -> bool {
    if (left.last_price_ != right.last_price_ || left.volume_ != right.volume_ ||
        left.upper_limit_price_ != right.upper_limit_price_ ||
        left.lower_limit_price_ != right.lower_limit_price_ ||
        left.open_interest_ != right.open_interest_) {
        return false;
    }
    for (std::size_t index = 0; index < Common::DEPTH_SNAPSHOT_LEVELS; ++index) {
        if (left.bids_[index].price_ != right.bids_[index].price_ ||
            left.bids_[index].qty_ != right.bids_[index].qty_ ||
            left.asks_[index].price_ != right.asks_[index].price_ ||
            left.asks_[index].qty_ != right.asks_[index].qty_) {
            return false;
        }
    }
    return true;
}

// Property test: replays 6000 seeded random depth events against both the
// pooled book and the retired ordered-map reference implementation,
// checking the published DEPTH_SNAPSHOT, level counts, lastUpdateId, and
// best bid/ask after every event.
TEST(BinanceDepthBookTest, PublishedUpdatesMatchTheReferenceModel) {
    std::mt19937 generator(20261012u);
    BinanceDepthBook book;
    DepthBookReference reference;

    nlohmann::json snapshot_bids = nlohmann::json::array();
    nlohmann::json snapshot_asks = nlohmann::json::array();
    for (int i = 0; i < 15; ++i) {
        snapshot_bids.push_back(priceLevel(gridPrice(99.0 + 0.1 * i).c_str(), "1.5"));
        snapshot_asks.push_back(priceLevel(gridPrice(101.0 + 0.1 * i).c_str(), "2.5"));
    }
    const auto snapshot = nlohmann::json{{"lastUpdateId", 1000},
                                         {"bids", snapshot_bids},
                                         {"asks", snapshot_asks}};
    book.installSnapshot(snapshot);
    reference.install(snapshot);

    std::vector<std::string> bid_prices;
    std::vector<std::string> ask_prices;
    for (int i = 0; i <= 30; ++i) {
        bid_prices.push_back(gridPrice(98.5 + 0.1 * i));
        ask_prices.push_back(gridPrice(100.5 + 0.1 * i));
    }
    const std::vector<std::string> quantities{"0", "1", "2.5", "0.75", "100", "7"};

    for (int event_index = 0; event_index < 6000; ++event_index) {
        const auto first = reference.last + 1;
        const auto final_update = first + 1 + (generator() % 5);
        nlohmann::json bids = nlohmann::json::array();
        nlohmann::json asks = nlohmann::json::array();
        for (int j = 0; j < 1 + static_cast<int>(generator() % 4); ++j) {
            bids.push_back(priceLevel(
                    bid_prices[generator() % bid_prices.size()].c_str(),
                    quantities[generator() % quantities.size()].c_str()));
        }
        for (int j = 0; j < 1 + static_cast<int>(generator() % 4); ++j) {
            asks.push_back(priceLevel(
                    ask_prices[generator() % ask_prices.size()].c_str(),
                    quantities[generator() % quantities.size()].c_str()));
        }
        const auto event = depthEvent(first, final_update, reference.last, bids, asks);
        book.onDepthEvent(event);
        reference.apply(event);

        const MarketUpdate published = book.buildMarketUpdate(0);
        const MarketUpdate expected = reference.publish(0);
        EXPECT_EQ(expected.type_, published.type_);
        EXPECT_EQ(expected.ticker_id_, published.ticker_id_);
        EXPECT_TRUE(sameDepthSnapshot(published.depth_snapshot_,
                                      expected.depth_snapshot_));
        EXPECT_EQ(reference.bids.size(), book.bidCount());
        EXPECT_EQ(reference.asks.size(), book.askCount());
        EXPECT_EQ(reference.last, book.lastUpdateId());
        if (!reference.bids.empty()) {
            ASSERT_TRUE(book.bestBid().has_value());
            EXPECT_EQ(reference.bids.rbegin()->first, book.bestBid()->price);
        }
        if (!reference.asks.empty()) {
            ASSERT_TRUE(book.bestAsk().has_value());
            EXPECT_EQ(reference.asks.begin()->first, book.bestAsk()->price);
        }
    }
}

}  // namespace
