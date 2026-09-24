#pragma once

#include <array>
#include <sstream>
#include <string>

#include "common/lf_queue.h"
#include "common/types.h"

namespace Exchange {

enum class MarketUpdateType : std::uint8_t {
  INVALID = 0,
  CLEAR = 1,
  ADD = 2,
  MODIFY = 3,
  CANCEL = 4,
  TRADE = 5,
  SNAPSHOT_START = 6,
  SNAPSHOT_END = 7,
  DEPTH_SNAPSHOT = 8,
};

inline auto marketUpdateTypeToString(MarketUpdateType type) -> std::string {
  switch (type) {
    case MarketUpdateType::INVALID:
      return "INVALID";
    case MarketUpdateType::CLEAR:
      return "CLEAR";
    case MarketUpdateType::ADD:
      return "ADD";
    case MarketUpdateType::MODIFY:
      return "MODIFY";
    case MarketUpdateType::CANCEL:
      return "CANCEL";
    case MarketUpdateType::TRADE:
      return "TRADE";
    case MarketUpdateType::SNAPSHOT_START:
      return "SNAPSHOT_START";
    case MarketUpdateType::SNAPSHOT_END:
      return "SNAPSHOT_END";
    case MarketUpdateType::DEPTH_SNAPSHOT:
      return "DEPTH_SNAPSHOT";
  }
  return "UNKNOWN";
}

struct PriceLevel {
  Common::Price price_ = Common::Price_INVALID;
  Common::Qty qty_ = Common::Qty_INVALID;
};

struct DepthSnapshot {
  Common::Price last_price_ = Common::Price_INVALID;
  std::array<PriceLevel, Common::DEPTH_SNAPSHOT_LEVELS> bids_{};
  std::array<PriceLevel, Common::DEPTH_SNAPSHOT_LEVELS> asks_{};
  Common::Qty volume_ = Common::Qty_INVALID;
  double open_interest_ = 0.0;
  Common::Price upper_limit_price_ = Common::Price_INVALID;
  Common::Price lower_limit_price_ = Common::Price_INVALID;
};

struct MarketUpdate {
  MarketUpdateType type_ = MarketUpdateType::INVALID;

  Common::OrderId order_id_ = Common::OrderId_INVALID;
  Common::TickerId ticker_id_ = Common::TickerId_INVALID;
  Common::Side side_ = Common::Side::INVALID;
  Common::Price price_ = Common::Price_INVALID;
  Common::Qty qty_ = Common::Qty_INVALID;
  Common::Priority priority_ = Common::Priority_INVALID;

  DepthSnapshot depth_snapshot_{};

  auto toString() const -> std::string {
    std::stringstream stream;
    stream << "MarketUpdate[type:" << marketUpdateTypeToString(type_)
           << " ticker:" << ticker_id_ << " order:" << order_id_
           << " side:" << Common::sideToString(side_)
           << " price:" << price_ << " qty:" << qty_ << "]";
    return stream.str();
  }
};

struct SequencedMarketUpdate {
  std::size_t seq_num_ = 0;
  MarketUpdate market_update_{};
};

using MarketUpdateLFQueue = Common::LFQueue<MarketUpdate>;

}  // namespace Exchange
