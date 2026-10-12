#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>

#include "common/lf_queue.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/jev_decision.h"
#include "trading/strategy/trade_engine.h"

namespace Tests {

constexpr auto kLongInterval = std::chrono::hours(1);

struct EngineHarness {
    Exchange::ClientRequestLFQueue requests;
    Exchange::ClientResponseLFQueue responses;
    Exchange::MarketUpdateLFQueue market_updates;
    Trading::JevDecisionLFQueue decisions;
    Trading::TradeEngine engine;

    explicit EngineHarness(Trading::RiskLimits limits, Common::ClientId client_id = 7)
            : requests(32), responses(32), market_updates(32), decisions(32),
              engine(client_id, limits, &requests, &responses, &market_updates, &decisions) {}

    static auto defaultLimits() -> Trading::RiskLimits {
        return {10, 10, 0.0};
    }
};

inline auto makeHarness(Trading::RiskLimits limits) -> std::unique_ptr<EngineHarness> {
    return std::make_unique<EngineHarness>(limits);
}

inline auto writeMarketUpdate(Exchange::MarketUpdateLFQueue& queue, Common::TickerId ticker,
                              Common::Price bid, Common::Price ask) -> void {
    auto* update = queue.getNextToWriteTo();
    update->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update->ticker_id_ = ticker;
    update->depth_snapshot_.bids_[0] = {bid, 2};
    update->depth_snapshot_.asks_[0] = {ask, 6};
    queue.updateWriteIndex();
}

inline auto writeDecision(Trading::JevDecisionLFQueue& queue, std::uint64_t evaluation_id,
                          Trading::JevBias bias = Trading::JevBias::LONG,
                          Trading::JevIntent intent = Trading::JevIntent::OPEN,
                          Common::TickerId ticker = 0) -> void {
    auto* decision = queue.getNextToWriteTo();
    decision->evaluation_id_ = evaluation_id;
    decision->ticker_id_ = ticker;
    decision->bias_ = bias;
    decision->intent_ = intent;
    queue.updateWriteIndex();
}

inline auto writeEvaluation(Trading::JevEvaluationStateLFQueue& queue, std::uint64_t evaluation_id)
        -> Trading::JevEvaluationState& {
    auto* state = queue.getNextToWriteTo();
    state->evaluation_id_ = evaluation_id;
    state->ticker_id_ = 0;
    return *state;
}

inline auto readRequest(Exchange::ClientRequestLFQueue& queue) -> Exchange::ClientRequest {
    const auto* request = queue.getNextToRead();
    EXPECT_NE(request, nullptr);
    const auto value = *request;
    queue.updateReadIndex();
    return value;
}

inline auto writeResponse(Exchange::ClientResponseLFQueue& queue,
                          Exchange::ClientResponseType type, Common::OrderId client_order_id,
                          Common::Side side, Common::Price price = 0, Common::Qty exec_qty = 0,
                          Common::Qty leaves_qty = 0,
                          Common::OrderOffset offset = Common::OrderOffset::OPEN,
                          Common::TickerId ticker = 0) -> void {
    auto* response = queue.getNextToWriteTo();
    response->type_ = type;
    response->client_id_ = 7;
    response->ticker_id_ = ticker;
    response->client_order_id_ = client_order_id;
    response->side_ = side;
    response->price_ = price;
    response->exec_qty_ = exec_qty;
    response->leaves_qty_ = leaves_qty;
    response->offset_ = offset;
    queue.updateWriteIndex();
}

inline auto writeFill(Exchange::ClientResponseLFQueue& queue, Common::ClientId client_id,
                      Common::OrderId client_order_id, Common::Side side, Common::Price price,
                      Common::Qty qty) -> void {
    auto* response = queue.getNextToWriteTo();
    response->type_ = Exchange::ClientResponseType::FILLED;
    response->client_id_ = client_id;
    response->ticker_id_ = 0;
    response->client_order_id_ = client_order_id;
    response->side_ = side;
    response->price_ = price;
    response->exec_qty_ = qty;
    response->leaves_qty_ = 0;
    response->offset_ = Common::OrderOffset::OPEN;
    queue.updateWriteIndex();
}

}  // namespace Tests
