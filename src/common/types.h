#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace Common {

constexpr std::size_t ME_MAX_TICKERS = 8;
constexpr std::size_t ME_MAX_CLIENT_UPDATES = 256 * 1024;
constexpr std::size_t ME_MAX_MARKET_UPDATES = 256 * 1024;
constexpr std::size_t ME_MAX_PRICE_LEVELS = 256;
constexpr std::size_t DEPTH_SNAPSHOT_LEVELS = 5;

using OrderId = std::uint64_t;
using TickerId = std::uint32_t;
using ClientId = std::uint32_t;
using Price = std::int64_t;
using Qty = std::uint32_t;
using Priority = std::uint64_t;

constexpr OrderId OrderId_INVALID = std::numeric_limits<OrderId>::max();
constexpr TickerId TickerId_INVALID = std::numeric_limits<TickerId>::max();
constexpr ClientId ClientId_INVALID = std::numeric_limits<ClientId>::max();
constexpr Price Price_INVALID = std::numeric_limits<Price>::max();
constexpr Qty Qty_INVALID = std::numeric_limits<Qty>::max();
constexpr Priority Priority_INVALID = std::numeric_limits<Priority>::max();

enum class Side : std::int8_t {
  INVALID = 0,
  BUY = 1,
  SELL = -1,
  MAX = 2,
};

enum class OrderType : std::uint8_t {
  INVALID = 0,
  LIMIT = 1,
  MARKET = 2,
};

enum class OrderOffset : std::uint8_t {
  INVALID = 0,
  OPEN = 1,
  CLOSE = 2,
  CLOSE_TODAY = 3,
  CLOSE_YESTERDAY = 4,
};

enum class PositionDay : std::uint8_t {
  TODAY = 0,
  YESTERDAY = 1,
  MAX = 2,
};

inline constexpr auto sideToIndex(Side side) noexcept -> std::size_t {
  return static_cast<std::size_t>(side) + 1;
}

inline constexpr auto sideToValue(Side side) noexcept -> int {
  return static_cast<int>(side);
}

inline auto sideToString(Side side) -> std::string {
  switch (side) {
    case Side::BUY:
      return "BUY";
    case Side::SELL:
      return "SELL";
    case Side::INVALID:
      return "INVALID";
    case Side::MAX:
      return "MAX";
  }
  return "UNKNOWN";
}

inline auto orderTypeToString(OrderType type) -> std::string {
  switch (type) {
    case OrderType::LIMIT:
      return "LIMIT";
    case OrderType::MARKET:
      return "MARKET";
    case OrderType::INVALID:
      return "INVALID";
  }
  return "UNKNOWN";
}

inline auto orderOffsetToString(OrderOffset offset) -> std::string {
  switch (offset) {
    case OrderOffset::OPEN:
      return "OPEN";
    case OrderOffset::CLOSE:
      return "CLOSE";
    case OrderOffset::CLOSE_TODAY:
      return "CLOSE_TODAY";
    case OrderOffset::CLOSE_YESTERDAY:
      return "CLOSE_YESTERDAY";
    case OrderOffset::INVALID:
      return "INVALID";
  }
  return "UNKNOWN";
}

inline auto positionDayToString(PositionDay day) -> std::string {
  switch (day) {
    case PositionDay::TODAY:
      return "TODAY";
    case PositionDay::YESTERDAY:
      return "YESTERDAY";
    case PositionDay::MAX:
      return "MAX";
  }
  return "UNKNOWN";
}

}  // namespace Common
