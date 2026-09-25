#pragma once

#include <array>
#include <functional>
#include <map>
#include <stdexcept>
#include <sstream>
#include <string>
#include <unordered_map>

#include "common/types.h"
#include "exchange/market_data/market_update.h"

namespace Trading {

struct BBO {
    Common::Price bid_price_ = Common::Price_INVALID;
    Common::Price ask_price_ = Common::Price_INVALID;
    Common::Qty bid_qty_ = Common::Qty_INVALID;
    Common::Qty ask_qty_ = Common::Qty_INVALID;

    auto toString() const -> std::string {
        std::stringstream stream;
        stream << "BBO[bid:" << bid_qty_ << "@" << bid_price_
                      << " ask:" << ask_price_ << "@" << ask_qty_ << "]";
        return stream.str();
    }
};

/// A participant-side local book that accepts both reference increments and
/// venue-provided depth snapshots.
class MarketOrderBook final {
  public:
    enum class BookMode : std::uint8_t {
        UNINITIALIZED = 0,
        INCREMENTAL = 1,
        DEPTH_SNAPSHOT = 2,
    };

    explicit MarketOrderBook(Common::TickerId ticker_id)
            : ticker_id_(ticker_id) {}

    MarketOrderBook(const MarketOrderBook&) = delete;
    MarketOrderBook& operator=(const MarketOrderBook&) = delete;
    MarketOrderBook(MarketOrderBook&&) = delete;
    MarketOrderBook& operator=(MarketOrderBook&&) = delete;

    auto onMarketUpdate(const Exchange::MarketUpdate& update) -> void {
        switch (update.type_) {
            case Exchange::MarketUpdateType::ADD:
                requireIncrementalMode();
                addOrder(update);
                break;
            case Exchange::MarketUpdateType::MODIFY:
                requireIncrementalMode();
                modifyOrder(update);
                break;
            case Exchange::MarketUpdateType::CANCEL:
                requireIncrementalMode();
                cancelOrder(update.order_id_);
                break;
            case Exchange::MarketUpdateType::CLEAR:
                requireIncrementalMode();
                clearIncrementalBook();
                break;
            case Exchange::MarketUpdateType::DEPTH_SNAPSHOT:
                requireDepthSnapshotMode();
                onDepthSnapshot(update.depth_snapshot_);
                return;
            case Exchange::MarketUpdateType::TRADE:
                requireIncrementalMode();
                return;
            case Exchange::MarketUpdateType::INVALID:
            case Exchange::MarketUpdateType::SNAPSHOT_START:
            case Exchange::MarketUpdateType::SNAPSHOT_END:
                return;
        }

        updateBBOFromIncrementalBook();
    }

    auto onDepthSnapshot(const Exchange::DepthSnapshot& snapshot) -> void {
        if (mode_ == BookMode::INCREMENTAL) {
            throw std::logic_error(
                    "MarketOrderBook cannot mix incremental and depth snapshot updates");
        }
        orders_.clear();
        bids_.clear();
        asks_.clear();
        depth_snapshot_ = snapshot;
        mode_ = BookMode::DEPTH_SNAPSHOT;
        updateBBOFromDepthSnapshot();
    }

    [[nodiscard]] auto getBBO() const noexcept -> const BBO* {
        return &bbo_;
    }

    [[nodiscard]] auto getDepthSnapshot() const noexcept
            -> const Exchange::DepthSnapshot* {
        return &depth_snapshot_;
    }

    [[nodiscard]] auto tickerId() const noexcept -> Common::TickerId {
        return ticker_id_;
    }

    [[nodiscard]] auto mode() const noexcept -> BookMode {
        return mode_;
    }

    [[nodiscard]] auto toString() const -> std::string {
        std::stringstream stream;
        stream << "MarketOrderBook[ticker:" << ticker_id_
                      << " mode:" << modeToString(mode_)
                      << " " << bbo_.toString() << "]";
        return stream.str();
    }

  private:
    struct BookOrder {
        Common::Side side_ = Common::Side::INVALID;
        Common::Price price_ = Common::Price_INVALID;
        Common::Qty qty_ = Common::Qty_INVALID;
    };

    const Common::TickerId ticker_id_;
    BookMode mode_ = BookMode::UNINITIALIZED;
    Exchange::DepthSnapshot depth_snapshot_{};
    BBO bbo_{};

    std::unordered_map<Common::OrderId, BookOrder> orders_;
    std::map<Common::Price, Common::Qty, std::greater<Common::Price>> bids_;
    std::map<Common::Price, Common::Qty> asks_;

    auto addOrder(const Exchange::MarketUpdate& update) -> void {
        cancelOrder(update.order_id_);
        orders_.emplace(update.order_id_,
                                        BookOrder{update.side_, update.price_, update.qty_});
        addLevel(update.side_, update.price_, update.qty_);
    }

    auto modifyOrder(const Exchange::MarketUpdate& update) -> void {
        const auto iterator = orders_.find(update.order_id_);
        if (iterator == orders_.end()) {
            return;
        }

        auto& order = iterator->second;
        if (order.side_ != update.side_ || order.price_ != update.price_) {
            removeLevel(order.side_, order.price_, order.qty_);
            order.side_ = update.side_;
            order.price_ = update.price_;
            addLevel(order.side_, order.price_, update.qty_);
        } else {
            if (update.qty_ >= order.qty_) {
                addLevel(order.side_, order.price_, update.qty_ - order.qty_);
            } else {
                removeLevel(order.side_, order.price_, order.qty_ - update.qty_);
            }
        }
        order.qty_ = update.qty_;
    }

    auto cancelOrder(Common::OrderId order_id) -> void {
        const auto iterator = orders_.find(order_id);
        if (iterator == orders_.end()) {
            return;
        }

        const auto& order = iterator->second;
        removeLevel(order.side_, order.price_, order.qty_);
        orders_.erase(iterator);
    }

    auto addLevel(Common::Side side, Common::Price price, Common::Qty qty)
            -> void {
        if (side == Common::Side::BUY) {
            bids_[price] += qty;
        } else {
            asks_[price] += qty;
        }
    }

    auto removeLevel(Common::Side side, Common::Price price, Common::Qty qty)
            -> void {
        if (side == Common::Side::BUY) {
            const auto iterator = bids_.find(price);
            if (iterator == bids_.end()) {
                return;
            }
            if (iterator->second <= qty) {
                bids_.erase(iterator);
            } else {
                iterator->second -= qty;
            }
        } else {
            const auto iterator = asks_.find(price);
            if (iterator == asks_.end()) {
                return;
            }
            if (iterator->second <= qty) {
                asks_.erase(iterator);
            } else {
                iterator->second -= qty;
            }
        }
    }

    auto clearIncrementalBook() -> void {
        orders_.clear();
        bids_.clear();
        asks_.clear();
        bbo_ = BBO{};
    }

    auto updateBBOFromIncrementalBook() -> void {
        bbo_.bid_price_ = bids_.empty() ? Common::Price_INVALID : bids_.begin()->first;
        bbo_.bid_qty_ = bids_.empty() ? Common::Qty_INVALID : bids_.begin()->second;
        bbo_.ask_price_ = asks_.empty() ? Common::Price_INVALID : asks_.begin()->first;
        bbo_.ask_qty_ = asks_.empty() ? Common::Qty_INVALID : asks_.begin()->second;
    }

    auto updateBBOFromDepthSnapshot() -> void {
        const auto& bid = depth_snapshot_.bids_.front();
        const auto& ask = depth_snapshot_.asks_.front();
        bbo_.bid_price_ = bid.price_;
        bbo_.bid_qty_ = bid.qty_;
        bbo_.ask_price_ = ask.price_;
        bbo_.ask_qty_ = ask.qty_;
    }

    static auto modeToString(BookMode mode) -> const char* {
        switch (mode) {
            case BookMode::UNINITIALIZED:
                return "UNINITIALIZED";
            case BookMode::INCREMENTAL:
                return "INCREMENTAL";
            case BookMode::DEPTH_SNAPSHOT:
                return "DEPTH_SNAPSHOT";
        }
        return "UNKNOWN";
    }

    auto requireIncrementalMode() -> void {
        if (mode_ == BookMode::DEPTH_SNAPSHOT) {
            throw std::logic_error(
                    "MarketOrderBook cannot mix depth snapshot and incremental updates");
        }
        mode_ = BookMode::INCREMENTAL;
    }

    auto requireDepthSnapshotMode() -> void {
        if (mode_ == BookMode::INCREMENTAL) {
            throw std::logic_error(
                    "MarketOrderBook cannot mix incremental and depth snapshot updates");
        }
        mode_ = BookMode::DEPTH_SNAPSHOT;
    }
};

using MarketOrderBookHashMap =
        std::array<MarketOrderBook*, Common::ME_MAX_TICKERS>;

}  // namespace Trading
