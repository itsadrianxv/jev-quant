#pragma once

#include <array>
#include <sstream>
#include <string>

#include "common/types.h"

namespace Trading {

enum class OMOrderState : std::int8_t {
  INVALID = 0,
  PENDING_NEW = 1,
  LIVE = 2,
  PENDING_CANCEL = 3,
  DEAD = 4,
};

inline auto omOrderStateToString(OMOrderState state) -> std::string {
  switch (state) {
    case OMOrderState::INVALID:
      return "INVALID";
    case OMOrderState::PENDING_NEW:
      return "PENDING_NEW";
    case OMOrderState::LIVE:
      return "LIVE";
    case OMOrderState::PENDING_CANCEL:
      return "PENDING_CANCEL";
    case OMOrderState::DEAD:
      return "DEAD";
  }
  return "UNKNOWN";
}

struct OMOrder {
  Common::TickerId ticker_id_ = Common::TickerId_INVALID;
  Common::OrderId order_id_ = Common::OrderId_INVALID;
  Common::Side side_ = Common::Side::INVALID;
  Common::Price price_ = Common::Price_INVALID;
  Common::Qty qty_ = Common::Qty_INVALID;
  Common::OrderType order_type_ = Common::OrderType::INVALID;
  Common::OrderOffset offset_ = Common::OrderOffset::INVALID;
  OMOrderState order_state_ = OMOrderState::INVALID;

  auto toString() const -> std::string {
    std::stringstream stream;
    stream << "OMOrder[ticker:" << ticker_id_ << " order:" << order_id_
           << " side:" << Common::sideToString(side_)
           << " price:" << price_ << " qty:" << qty_
           << " order-type:" << Common::orderTypeToString(order_type_)
           << " offset:" << Common::orderOffsetToString(offset_)
           << " state:" << omOrderStateToString(order_state_) << "]";
    return stream.str();
  }
};

using OMOrderSideHashMap =
    std::array<OMOrder, Common::sideToIndex(Common::Side::MAX) + 1>;
using OMOrderTickerSideHashMap =
    std::array<OMOrderSideHashMap, Common::ME_MAX_TICKERS>;

}  // namespace Trading
