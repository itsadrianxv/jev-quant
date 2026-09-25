#pragma once

#include <array>
#include <cstdint>

#include "common/lf_queue.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"
#include "trading/strategy/om_order.h"

namespace Trading {

struct PositionState {
    std::int32_t net_position_ = 0;
    Common::Qty today_qty_ = 0;
    Common::Qty yesterday_qty_ = 0;
    Common::Price average_entry_price_ = Common::Price_INVALID;
    double realized_pnl_ = 0.0;
    double unrealized_pnl_ = 0.0;
};

struct WorkingOrderState {
    Common::OrderId order_id_ = Common::OrderId_INVALID;
    Common::Side side_ = Common::Side::INVALID;
    Common::Price price_ = Common::Price_INVALID;
    Common::Qty qty_ = Common::Qty_INVALID;
    Common::OrderType order_type_ = Common::OrderType::INVALID;
    Common::OrderOffset offset_ = Common::OrderOffset::INVALID;
    OMOrderState state_ = OMOrderState::INVALID;
};

struct AccountState {
    double available_margin_ = 0.0;
    double used_margin_ = 0.0;
    double equity_ = 0.0;
};

struct RiskState {
    Common::Qty max_order_size_ = 0;
    Common::Qty max_position_ = 0;
    double max_loss_ = 0.0;
};

struct JevEvaluationState {
    std::uint64_t evaluation_id_ = 0;
    Common::TickerId ticker_id_ = Common::TickerId_INVALID;
    Exchange::DepthSnapshot depth_snapshot_{};
    PositionState position_{};
    std::array<WorkingOrderState, 2> working_orders_{};
    AccountState account_{};
    RiskState risk_{};
};

enum class JevBias : std::uint8_t {
    INVALID = 0,
    LONG = 1,
    SHORT = 2,
};

enum class JevIntent : std::uint8_t {
    INVALID = 0,
    OPEN = 1,
    CLOSE = 2,
    HOLD = 3,
};

struct JevDecision {
    std::uint64_t evaluation_id_ = 0;
    Common::TickerId ticker_id_ = Common::TickerId_INVALID;
    JevBias bias_ = JevBias::INVALID;
    JevIntent intent_ = JevIntent::INVALID;

    double long_probability_ = 0.0;
    double short_probability_ = 0.0;
    double open_probability_ = 0.0;
    double close_probability_ = 0.0;
    double hold_probability_ = 0.0;
    double confidence_ = 0.0;
};

using JevDecisionLFQueue = Common::LFQueue<JevDecision>;
using JevEvaluationStateLFQueue = Common::LFQueue<JevEvaluationState>;

}  // namespace Trading
