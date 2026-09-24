#include "trade_engine.h"

#include <exception>
#include <stdexcept>

namespace Trading {

TradeEngine::TradeEngine(
    Common::ClientId client_id, RiskLimits risk_limits,
    Exchange::ClientRequestLFQueue* outgoing_requests,
    Exchange::ClientResponseLFQueue* incoming_responses,
    Exchange::MarketUpdateLFQueue* incoming_market_updates,
    JevDecisionLFQueue* incoming_jev_decisions)
    : client_id_(client_id),
      position_keeper_(),
      risk_manager_(&position_keeper_, risk_limits),
      order_manager_(client_id_, this, &risk_manager_),
      outgoing_requests_(outgoing_requests),
      incoming_responses_(incoming_responses),
      incoming_market_updates_(incoming_market_updates),
      incoming_jev_decisions_(incoming_jev_decisions) {
  if (outgoing_requests_ == nullptr || incoming_responses_ == nullptr ||
      incoming_market_updates_ == nullptr || incoming_jev_decisions_ == nullptr) {
    throw std::invalid_argument("TradeEngine queues must not be null");
  }

  for (Common::TickerId ticker_id = 0;
       ticker_id < Common::ME_MAX_TICKERS; ++ticker_id) {
    ticker_order_books_[ticker_id] =
        std::make_unique<MarketOrderBook>(ticker_id);
  }
}

TradeEngine::TradeEngine(
    Exchange::ClientRequestLFQueue* outgoing_requests,
    Exchange::ClientResponseLFQueue* incoming_responses,
    Exchange::MarketUpdateLFQueue* incoming_market_updates,
    JevDecisionLFQueue* incoming_jev_decisions)
    : TradeEngine(0, defaultRiskLimits(), outgoing_requests, incoming_responses,
                  incoming_market_updates, incoming_jev_decisions) {}

auto TradeEngine::defaultRiskLimits() -> RiskLimits {
  return RiskLimits{Common::Qty_INVALID, Common::Qty_INVALID, 0.0};
}

TradeEngine::~TradeEngine() {
  stop();
}

auto TradeEngine::start() -> void {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return;
  }
  worker_ = std::thread([this] { run(); });
}

auto TradeEngine::stop() -> void {
  running_.store(false, std::memory_order_release);
  if (worker_.joinable()) {
    worker_.join();
  }
}

auto TradeEngine::run() -> void {
  while (running()) {
    if (!processPending()) {
      std::this_thread::yield();
    }
  }
}

auto TradeEngine::processPending() -> bool {
  bool processed = false;
  processed = processClientResponses() || processed;
  processed = processMarketUpdates() || processed;
  processed = processJevDecisions() || processed;
  return processed;
}

auto TradeEngine::processClientResponses() -> bool {
  bool processed = false;
  while (const auto* response = incoming_responses_->getNextToRead()) {
    handleClientResponse(*response);
    incoming_responses_->updateReadIndex();
    processed = true;
  }
  return processed;
}

auto TradeEngine::processMarketUpdates() -> bool {
  bool processed = false;
  while (const auto* update = incoming_market_updates_->getNextToRead()) {
    if (update->ticker_id_ >= ticker_order_books_.size()) {
      std::terminate();
    }

    auto& book = *ticker_order_books_.at(update->ticker_id_);
    book.onMarketUpdate(*update);
    handleMarketUpdate(*update, book);
    incoming_market_updates_->updateReadIndex();
    processed = true;
  }
  return processed;
}

auto TradeEngine::processJevDecisions() -> bool {
  bool processed = false;
  while (const auto* decision = incoming_jev_decisions_->getNextToRead()) {
    handleJevDecision(*decision);
    incoming_jev_decisions_->updateReadIndex();
    processed = true;
  }
  return processed;
}

auto TradeEngine::handleClientResponse(
    const Exchange::ClientResponse& response) -> void {
  if (response.ticker_id_ >= Common::ME_MAX_TICKERS) {
    return;
  }
  if (response.type_ == Exchange::ClientResponseType::FILLED) {
    position_keeper_.onClientResponse(response);
  }
  order_manager_.onOrderUpdate(response);
  onClientResponse(response);
}

auto TradeEngine::handleMarketUpdate(const Exchange::MarketUpdate& update,
                                     const MarketOrderBook& book) -> void {
  position_keeper_.updateBBO(update.ticker_id_, book.getBBO());
  onMarketUpdate(update, book);
}

auto TradeEngine::handleJevDecision(const JevDecision& decision) -> void {
  if (decision.ticker_id_ >= latest_evaluation_ids_.size() ||
      decision.evaluation_id_ == 0 ||
      decision.evaluation_id_ !=
          latest_evaluation_ids_.at(decision.ticker_id_)) {
    return;
  }
  onJevDecision(decision);
}

auto TradeEngine::registerEvaluation(Common::TickerId ticker_id,
                                     std::uint64_t evaluation_id) -> void {
  if (ticker_id >= latest_evaluation_ids_.size() || evaluation_id == 0) {
    throw std::invalid_argument("Invalid Jev evaluation registration");
  }
  latest_evaluation_ids_.at(ticker_id) = evaluation_id;
}

auto TradeEngine::buildJevEvaluationState(
    Common::TickerId ticker_id, std::uint64_t evaluation_id) const
    -> JevEvaluationState {
  if (ticker_id >= ticker_order_books_.size() || evaluation_id == 0) {
    throw std::invalid_argument("Invalid Jev evaluation state request");
  }

  JevEvaluationState state;
  state.evaluation_id_ = evaluation_id;
  state.ticker_id_ = ticker_id;
  state.depth_snapshot_ = *ticker_order_books_.at(ticker_id)->getDepthSnapshot();

  const auto& position = position_keeper_.getPositionInfo(ticker_id);
  state.position_.net_position_ = position.position_;
  state.position_.today_qty_ = position.todayQty();
  state.position_.yesterday_qty_ = position.yesterdayQty();
  state.position_.average_entry_price_ = position.averageEntryPrice();
  state.position_.realized_pnl_ = position.realized_pnl_;
  state.position_.unrealized_pnl_ = position.unrealized_pnl_;

  const auto& bid_order =
      order_manager_.getOrder(ticker_id, Common::Side::BUY);
  const auto& ask_order =
      order_manager_.getOrder(ticker_id, Common::Side::SELL);
  const auto toWorkingOrder = [](const OMOrder& order) {
    return WorkingOrderState{order.order_id_, order.side_, order.price_,
                             order.qty_, order.order_type_, order.offset_,
                             order.order_state_};
  };
  state.working_orders_ = {toWorkingOrder(bid_order),
                           toWorkingOrder(ask_order)};
  state.account_ = account_state_;
  const auto& limits = risk_manager_.limits();
  state.risk_ = RiskState{limits.max_order_size_, limits.max_position_,
                          limits.max_loss_};
  return state;
}

auto TradeEngine::sendClientRequest(const Exchange::ClientRequest& request)
    -> void {
  auto* slot = outgoing_requests_->tryGetNextToWriteTo();
  if (slot == nullptr) {
    std::terminate();
  }
  *slot = request;
  outgoing_requests_->updateWriteIndex();
}

}  // namespace Trading
