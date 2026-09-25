#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

#include "common/lf_queue.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "jev_decision.h"
#include "market_order_book.h"
#include "order_manager.h"
#include "position_keeper.h"
#include "risk_manager.h"

namespace Trading {

/// Participant-side event owner. All callbacks run on the TradeEngine thread
/// when processPending() or run() drains the queues.
class TradeEngine final {
  public:
    using ClientResponseHandler =
            std::function<void(const Exchange::ClientResponse&)>;
    using MarketUpdateHandler =
            std::function<void(const Exchange::MarketUpdate&, const MarketOrderBook&)>;
    using JevDecisionHandler = std::function<void(const JevDecision&)>;

    TradeEngine(Common::ClientId client_id, RiskLimits risk_limits,
                            Exchange::ClientRequestLFQueue* outgoing_requests,
                            Exchange::ClientResponseLFQueue* incoming_responses,
                            Exchange::MarketUpdateLFQueue* incoming_market_updates,
                            JevDecisionLFQueue* incoming_jev_decisions);

    TradeEngine(Exchange::ClientRequestLFQueue* outgoing_requests,
                            Exchange::ClientResponseLFQueue* incoming_responses,
                            Exchange::MarketUpdateLFQueue* incoming_market_updates,
                            JevDecisionLFQueue* incoming_jev_decisions);

    ~TradeEngine();

    TradeEngine(const TradeEngine&) = delete;
    TradeEngine& operator=(const TradeEngine&) = delete;
    TradeEngine(TradeEngine&&) = delete;
    TradeEngine& operator=(TradeEngine&&) = delete;

    auto start() -> void;
    auto stop() -> void;

    /// Drain available queues once in response, market, and Jev order.
    auto processPending() -> bool;

    /// Send a request to the OrderGateway queue.
    auto sendClientRequest(const Exchange::ClientRequest& request) -> void;

    auto registerEvaluation(Common::TickerId ticker_id,
                                                    std::uint64_t evaluation_id) -> void;

    auto attachJevEvaluationQueue(JevEvaluationStateLFQueue* evaluations,
                                                                std::chrono::milliseconds interval,
                                                                Common::TickerId ticker_id = 0) -> void;

    auto setDecisionOrderQuantity(Common::Qty quantity) -> void {
        if (quantity == 0 || quantity == Common::Qty_INVALID) {
            throw std::invalid_argument("Decision order quantity must be positive");
        }
        decision_order_quantity_ = quantity;
    }

    [[nodiscard]] auto buildJevEvaluationState(Common::TickerId ticker_id,
                                                                                            std::uint64_t evaluation_id) const
            -> JevEvaluationState;

    auto setAccountState(const AccountState& account_state) -> void {
        account_state_ = account_state;
    }

    [[nodiscard]] auto clientId() const noexcept -> Common::ClientId {
        return client_id_;
    }

    [[nodiscard]] auto positionKeeper() const noexcept -> const PositionKeeper& {
        return position_keeper_;
    }

    [[nodiscard]] auto orderManager() noexcept -> OrderManager& {
        return order_manager_;
    }

    [[nodiscard]] auto marketOrderBook(Common::TickerId ticker_id) noexcept
            -> MarketOrderBook& {
        return *ticker_order_books_.at(ticker_id);
    }

    [[nodiscard]] auto running() const noexcept -> bool {
        return running_.load(std::memory_order_acquire);
    }

    ClientResponseHandler onClientResponse = [](const Exchange::ClientResponse&) {};
    MarketUpdateHandler onMarketUpdate =
            [](const Exchange::MarketUpdate&, const MarketOrderBook&) {};
    JevDecisionHandler onJevDecision = [](const JevDecision&) {};

  private:
    static auto defaultRiskLimits() -> RiskLimits;

    auto run() -> void;
    auto processClientResponses() -> bool;
    auto processMarketUpdates() -> bool;
    auto processJevDecisions() -> bool;
    auto scheduleJevEvaluation() -> bool;
    auto handleClientResponse(const Exchange::ClientResponse& response) -> void;
    auto handleMarketUpdate(const Exchange::MarketUpdate& update,
                                                    const MarketOrderBook& book) -> void;
    auto handleJevDecision(const JevDecision& decision) -> void;

    const Common::ClientId client_id_ = Common::ClientId_INVALID;
    PositionKeeper position_keeper_;
    RiskManager risk_manager_;
    OrderManager order_manager_;
    AccountState account_state_{};
    std::array<std::uint64_t, Common::ME_MAX_TICKERS> latest_evaluation_ids_{};
    std::array<bool, Common::ME_MAX_TICKERS> instrument_ready_{};
    JevEvaluationStateLFQueue* outgoing_jev_evaluations_ = nullptr;
    std::chrono::milliseconds jev_evaluation_interval_{2000};
    std::chrono::steady_clock::time_point next_jev_evaluation_at_{};
    Common::TickerId jev_ticker_id_ = 0;
    std::uint64_t next_evaluation_id_ = 1;
    Common::Qty decision_order_quantity_ = 1;

    Exchange::ClientRequestLFQueue* outgoing_requests_ = nullptr;
    Exchange::ClientResponseLFQueue* incoming_responses_ = nullptr;
    Exchange::MarketUpdateLFQueue* incoming_market_updates_ = nullptr;
    JevDecisionLFQueue* incoming_jev_decisions_ = nullptr;

    std::array<std::unique_ptr<MarketOrderBook>, Common::ME_MAX_TICKERS>
            ticker_order_books_{};

    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace Trading
