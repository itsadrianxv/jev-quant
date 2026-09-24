#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "common/types.h"
#include "exchange/order_server/client_response.h"
#include "market_order_book.h"

namespace Trading {

struct PositionDayInfo {
  Common::Qty qty_ = 0;
  double open_vwap_ = 0.0;
};

struct PositionInfo {
  std::int32_t position_ = 0;
  std::array<PositionDayInfo,
             static_cast<std::size_t>(Common::PositionDay::MAX)>
      position_days_{};
  double realized_pnl_ = 0.0;
  double unrealized_pnl_ = 0.0;
  double total_pnl_ = 0.0;
  const BBO* bbo_ = nullptr;

  [[nodiscard]] auto todayQty() const noexcept -> Common::Qty {
    return position_days_[static_cast<std::size_t>(Common::PositionDay::TODAY)]
        .qty_;
  }

  [[nodiscard]] auto yesterdayQty() const noexcept -> Common::Qty {
    return position_days_[static_cast<std::size_t>(Common::PositionDay::YESTERDAY)]
        .qty_;
  }

  [[nodiscard]] auto averageEntryPrice() const noexcept -> Common::Price {
    if (position_ == 0) {
      return Common::Price_INVALID;
    }
    double cost = 0.0;
    for (const auto& position_day : position_days_) {
      cost += position_day.open_vwap_ * position_day.qty_;
    }
    return static_cast<Common::Price>(
        cost / static_cast<double>(std::abs(position_)));
  }

  [[nodiscard]] auto closeableQty(Common::OrderOffset offset) const noexcept
      -> Common::Qty {
    switch (offset) {
      case Common::OrderOffset::CLOSE_TODAY:
        return todayQty();
      case Common::OrderOffset::CLOSE_YESTERDAY:
        return yesterdayQty();
      case Common::OrderOffset::CLOSE:
        return todayQty() + yesterdayQty();
      case Common::OrderOffset::INVALID:
      case Common::OrderOffset::OPEN:
        return 0;
    }
    return 0;
  }

  [[nodiscard]] auto closeOffsetFor(Common::Qty requested_qty) const noexcept
      -> Common::OrderOffset {
    if (requested_qty == 0) {
      return Common::OrderOffset::INVALID;
    }
    if (yesterdayQty() > 0) {
      return Common::OrderOffset::CLOSE_YESTERDAY;
    }
    if (todayQty() > 0) {
      return Common::OrderOffset::CLOSE_TODAY;
    }
    return Common::OrderOffset::INVALID;
  }

  auto onFill(const Exchange::ClientResponse& response) -> void {
    if (response.exec_qty_ == 0 || response.exec_qty_ == Common::Qty_INVALID) {
      return;
    }

    if (response.offset_ == Common::OrderOffset::OPEN) {
      addOpen(response.exec_qty_, response.price_, Common::PositionDay::TODAY);
      position_ += Common::sideToValue(response.side_) *
                   static_cast<std::int32_t>(response.exec_qty_);
    } else {
      const auto closed_qty = closePosition(response.exec_qty_, response.price_,
                                            response.side_, response.offset_);
      position_ += Common::sideToValue(response.side_) *
                   static_cast<std::int32_t>(closed_qty);
    }

    recalculateUnrealizedPnl();
    total_pnl_ = realized_pnl_ + unrealized_pnl_;
  }

  auto updateBBO(const BBO* bbo) -> void {
    bbo_ = bbo;
    recalculateUnrealizedPnl();
    total_pnl_ = realized_pnl_ + unrealized_pnl_;
  }

  [[nodiscard]] auto toString() const -> std::string {
    std::stringstream stream;
    stream << "Position[position:" << position_ << " today:" << todayQty()
           << " yesterday:" << yesterdayQty() << " realized:" << realized_pnl_
           << " unrealized:" << unrealized_pnl_ << "]";
    return stream.str();
  }

 private:
  auto addOpen(Common::Qty qty, Common::Price price,
               Common::PositionDay day) -> void {
    auto& position_day = position_days_[static_cast<std::size_t>(day)];
    const auto old_qty = position_day.qty_;
    const auto new_qty = old_qty + qty;
    position_day.open_vwap_ =
        new_qty == 0
            ? 0.0
            : ((position_day.open_vwap_ * old_qty) +
               (static_cast<double>(price) * qty)) /
                  static_cast<double>(new_qty);
    position_day.qty_ = new_qty;
  }

  auto closePosition(Common::Qty requested_qty, Common::Price price,
                     Common::Side side, Common::OrderOffset offset)
      -> Common::Qty {
    auto remaining = requested_qty;
    auto closed = Common::Qty{0};

    const auto consume = [&](Common::PositionDay day, Common::Qty amount,
                             Common::Qty& remaining_ref,
                             Common::Qty& closed_ref) {
      auto& position_day = position_days_[static_cast<std::size_t>(day)];
      const auto consumed = std::min(position_day.qty_, amount);
      if (consumed == 0) {
        return;
      }
      const auto sign = Common::sideToValue(side);
      const auto pnl_per_unit = sign > 0
                                    ? position_day.open_vwap_ - price
                                    : price - position_day.open_vwap_;
      realized_pnl_ += pnl_per_unit * static_cast<double>(consumed);
      position_day.qty_ -= consumed;
      if (position_day.qty_ == 0) {
        position_day.open_vwap_ = 0.0;
      }
      remaining_ref -= consumed;
      closed_ref += consumed;
    };

    if (offset == Common::OrderOffset::CLOSE_TODAY) {
      consume(Common::PositionDay::TODAY, remaining, remaining, closed);
    } else if (offset == Common::OrderOffset::CLOSE_YESTERDAY) {
      consume(Common::PositionDay::YESTERDAY, remaining, remaining, closed);
    } else {
      consume(Common::PositionDay::YESTERDAY, remaining, remaining, closed);
      consume(Common::PositionDay::TODAY, remaining, remaining, closed);
    }

    return closed;
  }

  auto recalculateUnrealizedPnl() -> void {
    if (position_ == 0 || bbo_ == nullptr ||
        bbo_->bid_price_ == Common::Price_INVALID ||
        bbo_->ask_price_ == Common::Price_INVALID) {
      unrealized_pnl_ = 0.0;
      return;
    }

    const auto mid_price =
        (static_cast<double>(bbo_->bid_price_) +
         static_cast<double>(bbo_->ask_price_)) /
        2.0;
    double cost = 0.0;
    for (const auto& position_day : position_days_) {
      cost += position_day.open_vwap_ * position_day.qty_;
    }
    const auto average_price = cost / static_cast<double>(std::abs(position_));
    unrealized_pnl_ = position_ > 0
                          ? (mid_price - average_price) * std::abs(position_)
                          : (average_price - mid_price) * std::abs(position_);
  }
};

class PositionKeeper final {
 public:
  PositionKeeper() = default;

  auto onClientResponse(const Exchange::ClientResponse& response) -> void {
    if (response.type_ == Exchange::ClientResponseType::FILLED) {
      positions_.at(response.ticker_id_).onFill(response);
    }
  }

  auto updateBBO(Common::TickerId ticker_id, const BBO* bbo) -> void {
    positions_.at(ticker_id).updateBBO(bbo);
  }

  [[nodiscard]] auto getPositionInfo(Common::TickerId ticker_id) const
      -> const PositionInfo& {
    return positions_.at(ticker_id);
  }

  [[nodiscard]] auto getMutablePositionInfo(Common::TickerId ticker_id)
      -> PositionInfo& {
    return positions_.at(ticker_id);
  }

 private:
  std::array<PositionInfo, Common::ME_MAX_TICKERS> positions_{};
};

}  // namespace Trading
