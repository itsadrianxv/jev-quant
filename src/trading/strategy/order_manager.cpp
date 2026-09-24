#include "order_manager.h"

#include <cstdlib>

#include "trade_engine.h"

namespace Trading {

auto OrderManager::onOrderUpdate(const Exchange::ClientResponse& response)
    -> void {
  if (response.ticker_id_ >= orders_.size()) {
    return;
  }

  auto& order = orders_.at(response.ticker_id_).at(
      Common::sideToIndex(response.side_));
  switch (response.type_) {
    case Exchange::ClientResponseType::ACCEPTED:
      order.order_state_ = OMOrderState::LIVE;
      break;
    case Exchange::ClientResponseType::FILLED:
      order.qty_ = response.leaves_qty_;
      if (order.qty_ == 0) {
        order.order_state_ = OMOrderState::DEAD;
      }
      break;
    case Exchange::ClientResponseType::CANCELED:
    case Exchange::ClientResponseType::REJECTED:
      order.order_state_ = OMOrderState::DEAD;
      break;
    case Exchange::ClientResponseType::CANCEL_REJECTED:
    case Exchange::ClientResponseType::INVALID:
      break;
  }
}

auto OrderManager::moveOrders(Common::TickerId ticker_id,
                              Common::Price bid_price,
                              Common::Price ask_price, Common::Qty clip)
    -> void {
  auto& bid_order = orders_.at(ticker_id).at(Common::sideToIndex(Common::Side::BUY));
  auto& ask_order = orders_.at(ticker_id).at(Common::sideToIndex(Common::Side::SELL));
  moveOrder(bid_order, ticker_id, bid_price, Common::Side::BUY, clip,
            Common::OrderOffset::OPEN);
  moveOrder(ask_order, ticker_id, ask_price, Common::Side::SELL, clip,
            Common::OrderOffset::OPEN);
}

auto OrderManager::moveCloseOrder(Common::TickerId ticker_id,
                                  Common::Price price, Common::Side side,
                                  Common::Qty qty) -> void {
  if (risk_manager_ == nullptr) {
    return;
  }
  const auto offset = risk_manager_->resolveCloseOffset(ticker_id, qty);
  if (offset == Common::OrderOffset::INVALID) {
    return;
  }
  if (risk_manager_->checkPreTradeRisk(ticker_id, side, offset, qty) !=
      RiskCheckResult::ALLOWED) {
    return;
  }
  auto& order = orders_.at(ticker_id).at(Common::sideToIndex(side));
  moveOrder(order, ticker_id, price, side, qty, offset);
}

auto OrderManager::moveOrder(OMOrder& order, Common::TickerId ticker_id,
                             Common::Price price, Common::Side side,
                             Common::Qty qty, Common::OrderOffset offset)
    -> void {
  switch (order.order_state_) {
    case OMOrderState::LIVE:
      if (order.price_ != price || order.qty_ != qty || order.offset_ != offset) {
        cancelOrder(order);
      }
      break;
    case OMOrderState::INVALID:
    case OMOrderState::DEAD:
      if (price != Common::Price_INVALID && qty > 0) {
        newOrder(order, ticker_id, price, side, qty, offset);
      }
      break;
    case OMOrderState::PENDING_NEW:
    case OMOrderState::PENDING_CANCEL:
      break;
  }
}

auto OrderManager::newOrder(OMOrder& order, Common::TickerId ticker_id,
                            Common::Price price, Common::Side side,
                            Common::Qty qty, Common::OrderOffset offset)
    -> void {
  if (trade_engine_ == nullptr || risk_manager_ == nullptr ||
      risk_manager_->checkPreTradeRisk(ticker_id, side, offset, qty) !=
          RiskCheckResult::ALLOWED) {
    return;
  }

  Exchange::ClientRequest request;
  request.type_ = Exchange::ClientRequestType::NEW;
  request.client_id_ = client_id_;
  request.ticker_id_ = ticker_id;
  request.order_id_ = next_order_id_;
  request.side_ = side;
  request.price_ = price;
  request.qty_ = qty;
  request.order_type_ = Common::OrderType::LIMIT;
  request.offset_ = offset;
  trade_engine_->sendClientRequest(request);

  order.ticker_id_ = ticker_id;
  order.order_id_ = next_order_id_;
  order.side_ = side;
  order.price_ = price;
  order.qty_ = qty;
  order.order_type_ = request.order_type_;
  order.offset_ = offset;
  order.order_state_ = OMOrderState::PENDING_NEW;
  ++next_order_id_;
}

auto OrderManager::cancelOrder(OMOrder& order) -> void {
  if (trade_engine_ == nullptr || order.order_state_ != OMOrderState::LIVE) {
    return;
  }

  Exchange::ClientRequest request;
  request.type_ = Exchange::ClientRequestType::CANCEL;
  request.ticker_id_ = order.ticker_id_;
  request.order_id_ = order.order_id_;
  request.side_ = order.side_;
  request.price_ = order.price_;
  request.qty_ = order.qty_;
  request.order_type_ = order.order_type_;
  request.offset_ = order.offset_;
  trade_engine_->sendClientRequest(request);
  order.order_state_ = OMOrderState::PENDING_CANCEL;
}

}  // namespace Trading
