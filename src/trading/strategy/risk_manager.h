#pragma once

#include <cmath>
#include <cstdint>

#include "common/types.h"
#include "position_keeper.h"

namespace Trading {

enum class RiskCheckResult : std::uint8_t {
  INVALID = 0,
  ORDER_TOO_LARGE = 1,
  POSITION_TOO_LARGE = 2,
  CLOSE_QUANTITY_TOO_LARGE = 3,
  LOSS_TOO_LARGE = 4,
  ALLOWED = 5,
};

struct RiskLimits {
  Common::Qty max_order_size_ = Common::Qty_INVALID;
  Common::Qty max_position_ = Common::Qty_INVALID;
  double max_loss_ = 0.0;
};

class RiskManager final {
 public:
  RiskManager(const PositionKeeper* position_keeper, RiskLimits limits)
      : position_keeper_(position_keeper), limits_(limits) {}

  [[nodiscard]] auto checkPreTradeRisk(Common::TickerId ticker_id,
                                       Common::Side side,
                                       Common::OrderOffset offset,
                                       Common::Qty qty) const noexcept
      -> RiskCheckResult {
    if (position_keeper_ == nullptr || qty == 0 ||
        qty == Common::Qty_INVALID || side == Common::Side::INVALID) {
      return RiskCheckResult::INVALID;
    }

    if (limits_.max_order_size_ != Common::Qty_INVALID &&
        qty > limits_.max_order_size_) {
      return RiskCheckResult::ORDER_TOO_LARGE;
    }

    const auto& position = position_keeper_->getPositionInfo(ticker_id);
    if (offset == Common::OrderOffset::OPEN) {
      const auto next_position =
          position.position_ +
          Common::sideToValue(side) * static_cast<std::int32_t>(qty);
      if (limits_.max_position_ != Common::Qty_INVALID &&
          std::abs(next_position) >
              static_cast<std::int32_t>(limits_.max_position_)) {
        return RiskCheckResult::POSITION_TOO_LARGE;
      }
    } else if (qty > position.closeableQty(offset)) {
      return RiskCheckResult::CLOSE_QUANTITY_TOO_LARGE;
    }

    if (limits_.max_loss_ > 0.0 && position.total_pnl_ < -limits_.max_loss_) {
      return RiskCheckResult::LOSS_TOO_LARGE;
    }

    return RiskCheckResult::ALLOWED;
  }

  [[nodiscard]] auto resolveCloseOffset(Common::TickerId ticker_id,
                                         Common::Qty requested_qty) const noexcept
      -> Common::OrderOffset {
    if (position_keeper_ == nullptr || ticker_id >= Common::ME_MAX_TICKERS) {
      return Common::OrderOffset::INVALID;
    }
    return position_keeper_->getPositionInfo(ticker_id)
        .closeOffsetFor(requested_qty);
  }

  [[nodiscard]] auto limits() const noexcept -> const RiskLimits& {
    return limits_;
  }

 private:
  const PositionKeeper* position_keeper_ = nullptr;
  RiskLimits limits_{};
};

}  // namespace Trading
