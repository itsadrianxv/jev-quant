#pragma once

#include <bit>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "trading/strategy/market_order_book.h"

namespace Bench {
using Update = Exchange::MarketUpdate;
using Type = Exchange::MarketUpdateType;

inline auto require(bool condition, const std::string& message) -> void {
    if (!condition) throw std::runtime_error(message);
}

inline auto mix(std::uint64_t hash, std::uint64_t value) -> std::uint64_t {
    return (hash ^ value) * 1099511628211ULL;
}
inline auto digest(std::uint64_t hash, const Trading::BBO& bbo) -> std::uint64_t {
    for (auto value : {static_cast<std::uint64_t>(bbo.bid_price_),
                       static_cast<std::uint64_t>(bbo.ask_price_),
                       static_cast<std::uint64_t>(bbo.bid_qty_),
                       static_cast<std::uint64_t>(bbo.ask_qty_)}) hash = mix(hash, value);
    return hash;
}
inline constexpr std::uint64_t kHashStart = 14695981039346656037ULL;

inline auto snapshotDigest(std::uint64_t hash, const Exchange::DepthSnapshot& s) -> std::uint64_t {
    for (const auto& levels : {s.bids_, s.asks_})
        for (const auto& level : levels) { hash = mix(hash, level.price_); hash = mix(hash, level.qty_); }
    hash = mix(hash, s.last_price_);
    hash = mix(hash, s.volume_);
    hash = mix(hash, std::bit_cast<std::uint64_t>(s.open_interest_));
    hash = mix(hash, s.upper_limit_price_);
    return mix(hash, s.lower_limit_price_);
}
inline auto snapshotEqual(const Exchange::DepthSnapshot& a, const Exchange::DepthSnapshot& b) -> bool {
    for (std::size_t i = 0; i < Common::DEPTH_SNAPSHOT_LEVELS; ++i)
        if (a.bids_[i].price_ != b.bids_[i].price_ || a.bids_[i].qty_ != b.bids_[i].qty_ ||
            a.asks_[i].price_ != b.asks_[i].price_ || a.asks_[i].qty_ != b.asks_[i].qty_) return false;
    return a.last_price_ == b.last_price_ && a.volume_ == b.volume_ &&
           a.open_interest_ == b.open_interest_ && a.upper_limit_price_ == b.upper_limit_price_ &&
           a.lower_limit_price_ == b.lower_limit_price_;
}

inline auto snapshot(Common::Price bid, std::size_t depth, Common::Qty qty) -> Update {
    Update u;
    u.type_ = Type::DEPTH_SNAPSHOT;
    u.ticker_id_ = 0;
    u.depth_snapshot_.last_price_ = bid;
    for (std::size_t i = 0; i < depth; ++i) {
        u.depth_snapshot_.bids_[i] = {bid - static_cast<Common::Price>(i), qty};
        u.depth_snapshot_.asks_[i] = {bid + 1 + static_cast<Common::Price>(i), qty + 1};
    }
    return u;
}
inline auto incremental(Type type, std::uint64_t id, Common::Side side,
                        Common::Price price, Common::Qty qty) -> Update {
    Update u;
    u.type_ = type;
    u.ticker_id_ = 0;
    u.order_id_ = id;
    u.side_ = side;
    u.price_ = price;
    u.qty_ = qty;
    return u;
}

/// Independent ordered-map oracle. Used only during input preparation and the
/// untimed correctness pass, never in the measured book loop.
class ReferenceBook {
    std::unordered_map<std::uint64_t, Update> orders_;
    std::map<Common::Price, Common::Qty> bids_, asks_;
  public:
    auto apply(const Update& u) -> Trading::BBO {
        if (u.type_ == Type::DEPTH_SNAPSHOT) {
            const auto& b = u.depth_snapshot_.bids_[0];
            const auto& a = u.depth_snapshot_.asks_[0];
            return {b.price_, a.price_, b.qty_, a.qty_};
        }
        if (u.type_ == Type::CLEAR) { orders_.clear(); bids_.clear(); asks_.clear(); }
        if (u.type_ == Type::MODIFY || u.type_ == Type::CANCEL) {
            const auto old = orders_.at(u.order_id_);
            auto& levels = old.side_ == Common::Side::BUY ? bids_ : asks_;
            auto it = levels.find(old.price_);
            it->second -= old.qty_;
            if (!it->second) levels.erase(it);
            orders_.erase(u.order_id_);
        }
        if (u.type_ == Type::ADD || u.type_ == Type::MODIFY) {
            require(!orders_.contains(u.order_id_), "duplicate generated order");
            orders_[u.order_id_] = u;
            (u.side_ == Common::Side::BUY ? bids_ : asks_)[u.price_] += u.qty_;
        }
        Trading::BBO bbo;
        if (!bids_.empty()) { bbo.bid_price_ = bids_.rbegin()->first; bbo.bid_qty_ = bids_.rbegin()->second; }
        if (!asks_.empty()) { bbo.ask_price_ = asks_.begin()->first; bbo.ask_qty_ = asks_.begin()->second; }
        return bbo;
    }
};

struct Workload {
    std::vector<Update> setup, events;
    std::vector<Trading::BBO> expected;
    std::uint64_t hash = kHashStart;
};

inline auto generate(const std::string& scenario, std::size_t count,
                     std::uint64_t seed) -> Workload {
    Workload w;
    w.events.reserve(count);
    w.expected.reserve(count);
    std::mt19937_64 rng(seed);
    const bool inc = scenario.starts_with("incremental-");
    const bool boundary = scenario.ends_with("boundary");
    std::size_t population = 0;
    if (inc && !boundary) population = std::stoull(scenario.substr(12));
    const auto resting = [population](std::uint64_t id) {
        const bool buy = id <= population / 2;
        const auto level = (id - 1) % (population / 8);
        return incremental(Type::ADD, id, buy ? Common::Side::BUY : Common::Side::SELL,
                buy ? 10000 - static_cast<Common::Price>(level)
                    : 10001 + static_cast<Common::Price>(level), 2);
    };
    for (std::size_t id = 1; id <= population; ++id) w.setup.push_back(resting(id));
    std::uint64_t pending_id = 1;
    for (std::size_t i = 0; i < count; ++i) {
        Update u;
        if (inc && boundary) {
            const auto step = i % 8;
            if (step == 0) u = incremental(Type::CLEAR, 0, Common::Side::BUY, 0, 0);
            if (step == 1 || step == 4) u = incremental(Type::ADD, 1, Common::Side::BUY, 100, 2);
            if (step == 2 || step == 6) u = incremental(Type::ADD, 2, Common::Side::SELL, 101, 3);
            if (step == 3) u = incremental(Type::CANCEL, 1, Common::Side::BUY, 100, 2);
            if (step == 5) u = incremental(Type::CANCEL, 2, Common::Side::SELL, 101, 3);
            if (step == 7) u = incremental(Type::MODIFY, 1, Common::Side::BUY, 100, 4);
        } else if (inc) {
            if (i % 4 != 2) pending_id = 1 + rng() % population;
            u = resting(pending_id);
            u.type_ = i % 4 == 1 ? Type::CANCEL : i % 4 == 2 ? Type::ADD : Type::MODIFY;
            u.qty_ = 1 + rng() % 20;
        } else {
            const bool pipeline = scenario.starts_with("pipeline-");
            auto bid = pipeline ? 100 + static_cast<Common::Price>(i % 2)
                                : 100 + static_cast<Common::Price>(rng() % 8);
            // A constant quote for fills produces a known spread loss per pair.
            if (scenario == "pipeline-fill") bid = 100;
            u = snapshot(bid, scenario == "snapshot-5" ? 5 : 1, 1 + rng() % 10);
            if (boundary && i % 4 == 1) u.depth_snapshot_.asks_ = {};
            if (boundary && i % 4 == 2) { u.depth_snapshot_.asks_ = {}; u.depth_snapshot_.bids_ = {}; }
        }
        w.events.push_back(u);
    }
    ReferenceBook reference;
    const auto hash_update = [&](const Update& u) {
        for (auto value : {static_cast<std::uint64_t>(u.type_), u.order_id_,
                          static_cast<std::uint64_t>(u.side_), static_cast<std::uint64_t>(u.price_),
                          static_cast<std::uint64_t>(u.qty_)}) w.hash = mix(w.hash, value);
        w.hash = snapshotDigest(w.hash, u.depth_snapshot_);
    };
    for (const auto& u : w.setup) { reference.apply(u); hash_update(u); }
    for (const auto& u : w.events) { w.expected.push_back(reference.apply(u)); hash_update(u); }
    return w;
}

inline auto equal(const Trading::BBO& a, const Trading::BBO& b) -> bool {
    return a.bid_price_ == b.bid_price_ && a.ask_price_ == b.ask_price_ &&
           a.bid_qty_ == b.bid_qty_ && a.ask_qty_ == b.ask_qty_;
}
}  // namespace Bench
