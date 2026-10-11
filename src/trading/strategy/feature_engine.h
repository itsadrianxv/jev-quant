#pragma once

#include <cmath>
#include <cstdint>

#include "trading/strategy/market_order_book.h"

namespace Trading {

struct MarketFeatures {
    double mid_price_ = 0.0;
    double relative_spread_bps_ = 0.0;
    std::uint64_t top_of_book_total_qty_ = 0;
    double top_of_book_qty_imbalance_ = 0.0;
    bool valid_ = false;
};

class FeatureEngine final {
  public:
    auto onMarketUpdate(const MarketOrderBook& book) noexcept -> void {
        const auto* bbo = book.getBBO();
        if (bbo == nullptr || bbo->bid_price_ == Common::Price_INVALID ||
            bbo->ask_price_ == Common::Price_INVALID || bbo->bid_qty_ == Common::Qty_INVALID ||
            bbo->ask_qty_ == Common::Qty_INVALID || bbo->bid_price_ <= 0 ||
            bbo->ask_price_ <= bbo->bid_price_ || bbo->bid_qty_ == 0 || bbo->ask_qty_ == 0) {
            features_ = {};
            return;
        }

        const auto bid_qty = static_cast<double>(bbo->bid_qty_);
        const auto ask_qty = static_cast<double>(bbo->ask_qty_);
        const auto total_qty = bid_qty + ask_qty;
        const auto mid_price = (static_cast<double>(bbo->bid_price_) +
                                static_cast<double>(bbo->ask_price_)) /
                               2.0;
        if (!std::isfinite(mid_price) || mid_price <= 0.0 || !std::isfinite(total_qty) ||
            total_qty <= 0.0) {
            features_ = {};
            return;
        }

        features_.mid_price_ = mid_price;
        features_.relative_spread_bps_ =
                (static_cast<double>(bbo->ask_price_ - bbo->bid_price_) / mid_price) * 10000.0;
        features_.top_of_book_total_qty_ = static_cast<std::uint64_t>(bbo->bid_qty_) + bbo->ask_qty_;
        features_.top_of_book_qty_imbalance_ = (bid_qty - ask_qty) / total_qty;
        features_.valid_ = std::isfinite(features_.relative_spread_bps_) &&
                           std::isfinite(features_.top_of_book_qty_imbalance_);
        if (!features_.valid_) features_ = {};
    }

    [[nodiscard]] auto features() const noexcept -> const MarketFeatures& { return features_; }

  private:
    // TODO: Add freshness tracking when market timestamp semantics are defined.
    MarketFeatures features_{};
};

}  // namespace Trading
