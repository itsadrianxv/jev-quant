#pragma once

#include <array>

#include "common/types.h"
#include "exchange/order_server/client_response.h"
#include "om_order.h"
#include "risk_manager.h"

namespace Trading {

class TradeEngine;

class OrderManager final {
  public:
    OrderManager(Common::ClientId client_id, TradeEngine* trade_engine,
                              const RiskManager* risk_manager)
            : client_id_(client_id),
                trade_engine_(trade_engine),
                risk_manager_(risk_manager) {}

    auto onOrderUpdate(const Exchange::ClientResponse& response) -> void;

    auto moveOrders(Common::TickerId ticker_id, Common::Price bid_price,
                                    Common::Price ask_price, Common::Qty clip) -> void;

    auto moveOpenOrder(Common::TickerId ticker_id, Common::Price price,
                                          Common::Side side, Common::Qty qty) -> void;

    auto moveCloseOrder(Common::TickerId ticker_id, Common::Price price,
                                            Common::Side side, Common::Qty qty) -> void;

    [[nodiscard]] auto getOrder(Common::TickerId ticker_id, Common::Side side)
            -> OMOrder& {
        return orders_.at(ticker_id).at(Common::sideToIndex(side));
    }

    [[nodiscard]] auto getOrder(Common::TickerId ticker_id,
                                                            Common::Side side) const -> const OMOrder& {
        return orders_.at(ticker_id).at(Common::sideToIndex(side));
    }

  private:
    auto moveOrder(OMOrder& order, Common::TickerId ticker_id,
                                  Common::Price price, Common::Side side, Common::Qty qty,
                                  Common::OrderOffset offset) -> void;
    auto newOrder(OMOrder& order, Common::TickerId ticker_id,
                                Common::Price price, Common::Side side, Common::Qty qty,
                                Common::OrderOffset offset) -> void;
    auto cancelOrder(OMOrder& order) -> void;

    const Common::ClientId client_id_ = Common::ClientId_INVALID;
    TradeEngine* trade_engine_ = nullptr;
    const RiskManager* risk_manager_ = nullptr;
    OMOrderTickerSideHashMap orders_{};
    Common::OrderId next_order_id_ = 1;
};

}  // namespace Trading
