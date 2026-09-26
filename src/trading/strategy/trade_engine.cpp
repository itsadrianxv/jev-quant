#include "trade_engine.h"

#include <exception>
#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace Trading {

TradeEngine::TradeEngine(
        Common::ClientId client_id, RiskLimits risk_limits,
        Exchange::ClientRequestLFQueue* outgoing_requests,
        Exchange::ClientResponseLFQueue* incoming_responses,
        Exchange::MarketUpdateLFQueue* incoming_market_updates,
        JevDecisionLFQueue* incoming_jev_decisions,
        Common::AsyncLogger* logger)
        : client_id_(client_id),
            position_keeper_(),
            risk_manager_(&position_keeper_, risk_limits),
            order_manager_(client_id_, this, &risk_manager_),
            outgoing_requests_(outgoing_requests),
            incoming_responses_(incoming_responses),
            incoming_market_updates_(incoming_market_updates),
            incoming_jev_decisions_(incoming_jev_decisions), logger_(logger) {
    if (outgoing_requests_ == nullptr || incoming_responses_ == nullptr ||
            incoming_market_updates_ == nullptr || incoming_jev_decisions_ == nullptr) {
        throw std::invalid_argument("TradeEngine queues must not be null");
    }

    for (Common::TickerId ticker_id = 0;
              ticker_id < Common::ME_MAX_TICKERS; ++ticker_id) {
        ticker_order_books_[ticker_id] =
                std::make_unique<MarketOrderBook>(ticker_id);
    }
    next_jev_evaluation_at_ = std::chrono::steady_clock::now();
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
    if (logger_ != nullptr) {
        log_handle_.emplace(logger_->registerProducer(
                "TradeEngine", "trade-engine-" + std::to_string(client_id_) + ".log"));
        log_handle_->bindToCurrentThread();
        log_handle_->log(Common::LogLevel::INFO,
                         "event=component_started client_id=" + std::to_string(client_id_));
    }
    while (running()) {
        if (!processPending()) {
            std::this_thread::yield();
        }
    }
    if (log_handle_) {
        log_handle_->log(Common::LogLevel::INFO,
                         "event=component_stopped client_id=" + std::to_string(client_id_));
    }
}

auto TradeEngine::processPending() -> bool {
    bool processed = false;
    processed = processClientResponses() || processed;
    processed = processMarketUpdates() || processed;
    processed = processJevDecisions() || processed;
    processed = scheduleJevEvaluation() || processed;
    return processed;
}

auto TradeEngine::processClientResponses() -> bool {
    bool processed = false;
    while (const auto* response = incoming_responses_->getNextToRead()) {
        if (log_handle_) {
            log_handle_->log(Common::LogLevel::INFO,
                             "event=client_response_consumed client_id=" +
                                     std::to_string(client_id_) + " ticker_id=" +
                                     std::to_string(response->ticker_id_));
        }
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
        if (log_handle_ && (verbose_market_data_ ||
                            update->type_ == Exchange::MarketUpdateType::DEPTH_SNAPSHOT)) {
            log_handle_->log(Common::LogLevel::DEBUG,
                             "event=market_update_consumed client_id=" +
                                     std::to_string(client_id_) + " ticker_id=" +
                                     std::to_string(update->ticker_id_));
        }
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
        if (log_handle_) {
            log_handle_->log(Common::LogLevel::INFO,
                             "event=decision_consumed client_id=" +
                                     std::to_string(client_id_) + " evaluation_id=" +
                                     std::to_string(decision->evaluation_id_));
        }
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
        const auto expected_position = static_cast<std::int64_t>(position_keeper_.getPositionInfo(response.ticker_id_).position_) +
                Common::sideToValue(response.side_) * static_cast<std::int64_t>(response.exec_qty_);
        position_keeper_.onClientResponse(response);
        if (simex_constraints_) {
            const auto& position = position_keeper_.getPositionInfo(response.ticker_id_);
            if (position.position_ != expected_position ||
                static_cast<std::uint64_t>(position.todayQty()) + position.yesterdayQty() !=
                    static_cast<std::uint64_t>(std::abs(expected_position)))
                throw std::logic_error("Simex execution and participant position disagree");
        }
    }
    order_manager_.onOrderUpdate(response);
    onClientResponse(response);
}

auto TradeEngine::handleMarketUpdate(const Exchange::MarketUpdate& update,
                                                                          const MarketOrderBook& book) -> void {
    position_keeper_.updateBBO(update.ticker_id_, book.getBBO());
    if (update.type_ == Exchange::MarketUpdateType::DEPTH_SNAPSHOT &&
            update.ticker_id_ < instrument_ready_.size()) {
        const auto* bbo = book.getBBO();
        const bool ready = !simex_constraints_ ||
                (bbo->bid_price_ > 0 && bbo->bid_price_ != Common::Price_INVALID &&
                 bbo->ask_price_ > bbo->bid_price_ && bbo->ask_price_ != Common::Price_INVALID &&
                 bbo->bid_qty_ > 0 && bbo->bid_qty_ != Common::Qty_INVALID &&
                 bbo->ask_qty_ > 0 && bbo->ask_qty_ != Common::Qty_INVALID);
        instrument_ready_.at(update.ticker_id_) = ready;
        if (!ready) latest_evaluation_ids_.at(update.ticker_id_) = 0;
    }
    onMarketUpdate(update, book);
}

auto TradeEngine::handleJevDecision(const JevDecision& decision) -> void {
    if (decision.ticker_id_ >= latest_evaluation_ids_.size() ||
            decision.evaluation_id_ == 0 ||
            decision.evaluation_id_ !=
                    latest_evaluation_ids_.at(decision.ticker_id_)) {
        if (log_handle_) {
            log_handle_->log(Common::LogLevel::WARN,
                             "event=stale_decision_dropped client_id=" +
                                     std::to_string(client_id_) + " evaluation_id=" +
                                     std::to_string(decision.evaluation_id_));
        }
        return;
    }

    if (simex_constraints_ && !instrument_ready_.at(decision.ticker_id_)) return;

    const auto& book = *ticker_order_books_.at(decision.ticker_id_);
    const auto* bbo = book.getBBO();
    if (decision.intent_ == JevIntent::OPEN) {
        const auto side = decision.bias_ == JevBias::LONG ? Common::Side::BUY
                                                                                                              : Common::Side::SELL;
        if (simex_constraints_) {
            const auto position = position_keeper_.getPositionInfo(decision.ticker_id_).position_;
            const auto& opposite = order_manager_.getOrder(decision.ticker_id_,
                    side == Common::Side::BUY ? Common::Side::SELL : Common::Side::BUY);
            const bool outstanding = opposite.order_state_ != OMOrderState::INVALID &&
                                     opposite.order_state_ != OMOrderState::DEAD &&
                                     opposite.offset_ == Common::OrderOffset::OPEN;
            if ((position > 0 && side == Common::Side::SELL) ||
                (position < 0 && side == Common::Side::BUY) || outstanding) {
                if (log_handle_) log_handle_->log(Common::LogLevel::WARN, "event=opposite_open_blocked");
                onJevDecision(decision);
                return;
            }
        }
        // TODO(simex-counterparty): After observing replenishment and fills, decide
        // whether simex orders should keep crossing at the current best quote or
        // rest passively, and define their reprice/cancel lifetime. Keep the current
        // LIMIT behavior until that execution policy is agreed; Jev still owns intent.
        const auto price = side == Common::Side::BUY ? bbo->ask_price_
                                                                                                    : bbo->bid_price_;
        if (price != Common::Price_INVALID) {
            order_manager_.moveOpenOrder(decision.ticker_id_, price, side,
                                                                      decision_order_quantity_);
        }
    } else if (decision.intent_ == JevIntent::CLOSE) {
        const auto& position = position_keeper_.getPositionInfo(decision.ticker_id_);
        if (position.position_ != 0) {
            const auto side = position.position_ > 0 ? Common::Side::SELL
                                                                                                : Common::Side::BUY;
            const auto price = side == Common::Side::BUY ? bbo->ask_price_
                                                                                                        : bbo->bid_price_;
            if (price != Common::Price_INVALID) {
                order_manager_.moveCloseOrder(decision.ticker_id_, price, side,
                                                                            decision_order_quantity_);
            }
        }
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

auto TradeEngine::attachJevEvaluationQueue(
        JevEvaluationStateLFQueue* evaluations,
        std::chrono::milliseconds interval, Common::TickerId ticker_id) -> void {
    if (evaluations == nullptr || interval.count() < 0 ||
            ticker_id >= ticker_order_books_.size()) {
        throw std::invalid_argument("Invalid Jev evaluation queue configuration");
    }
    outgoing_jev_evaluations_ = evaluations;
    jev_evaluation_interval_ = interval;
    jev_ticker_id_ = ticker_id;
    next_jev_evaluation_at_ = std::chrono::steady_clock::now();
}

auto TradeEngine::scheduleJevEvaluation() -> bool {
    if (outgoing_jev_evaluations_ == nullptr ||
            !instrument_ready_.at(jev_ticker_id_) ||
            std::chrono::steady_clock::now() < next_jev_evaluation_at_) {
        return false;
    }

    const auto evaluation_id = next_evaluation_id_++;
    auto state = buildJevEvaluationState(jev_ticker_id_, evaluation_id);
    auto* slot = outgoing_jev_evaluations_->tryGetNextToWriteTo();
    if (slot == nullptr) {
        std::terminate();
    }
    registerEvaluation(jev_ticker_id_, evaluation_id);
    *slot = state;
    outgoing_jev_evaluations_->updateWriteIndex();
    if (log_handle_) {
        log_handle_->log(Common::LogLevel::INFO,
                         "event=evaluation_enqueued client_id=" +
                                 std::to_string(client_id_) + " evaluation_id=" +
                                 std::to_string(evaluation_id) + " ticker_id=" +
                                 std::to_string(jev_ticker_id_));
    }
    next_jev_evaluation_at_ = std::chrono::steady_clock::now() +
                                                          jev_evaluation_interval_;
    return true;
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
    if (log_handle_) {
        log_handle_->log(Common::LogLevel::INFO,
                         "event=client_request_enqueued client_id=" +
                                 std::to_string(client_id_) + " ticker_id=" +
                                 std::to_string(request.ticker_id_));
    }
}

}  // namespace Trading
