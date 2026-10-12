#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>

#include "common/lf_queue.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "../simulated_venue.h"
#include "../failing_jev_provider.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"
#include "trading/strategy/feature_engine.h"
#include "trading/strategy/jev_worker.h"
#include "trading/strategy/market_order_book.h"
#include "trading/strategy/om_order.h"
#include "trading/strategy/trade_engine.h"

namespace {

using Trading::JevBias;
using Trading::JevDecision;
using Trading::JevEvaluationState;
using Trading::JevIntent;
using Trading::JevWorker;
using Trading::MockJevDecisionProvider;
using Trading::OMOrderState;
using Tests::FailingJevProvider;
using Tests::SimulatedVenue;

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

auto makeHarness(Trading::RiskLimits limits) -> std::unique_ptr<EngineHarness> {
    return std::make_unique<EngineHarness>(limits);
}

auto writeMarketUpdate(Exchange::MarketUpdateLFQueue& queue, Common::TickerId ticker,
                       Common::Price bid, Common::Price ask) -> void {
    auto* update = queue.getNextToWriteTo();
    update->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update->ticker_id_ = ticker;
    update->depth_snapshot_.bids_[0] = {bid, 2};
    update->depth_snapshot_.asks_[0] = {ask, 6};
    queue.updateWriteIndex();
}

auto writeDecision(Trading::JevDecisionLFQueue& queue, std::uint64_t evaluation_id,
                   JevBias bias = JevBias::LONG, JevIntent intent = JevIntent::OPEN)
        -> void {
    auto* decision = queue.getNextToWriteTo();
    decision->evaluation_id_ = evaluation_id;
    decision->ticker_id_ = 0;
    decision->bias_ = bias;
    decision->intent_ = intent;
    queue.updateWriteIndex();
}

auto writeEvaluation(Trading::JevEvaluationStateLFQueue& queue, std::uint64_t evaluation_id)
        -> JevEvaluationState& {
    auto* state = queue.getNextToWriteTo();
    state->evaluation_id_ = evaluation_id;
    state->ticker_id_ = 0;
    return *state;
}

auto readRequest(Exchange::ClientRequestLFQueue& queue) -> Exchange::ClientRequest {
    const auto* request = queue.getNextToRead();
    EXPECT_NE(request, nullptr);
    const auto value = *request;
    queue.updateReadIndex();
    return value;
}

auto writeFill(Exchange::ClientResponseLFQueue& queue, Common::ClientId client_id,
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

TEST(TradeEngineDispatchTest, DrainsResponsesThenMarketUpdatesThenDecisions) {
    auto harness = makeHarness({10, 100, 0.0});
    auto& engine = harness->engine;

    std::string order;
    engine.onClientResponse = [&](const Exchange::ClientResponse&) { order += 'R'; };
    engine.onMarketUpdate = [&](const Exchange::MarketUpdate&, const Trading::MarketOrderBook&) {
        order += 'M';
    };
    engine.onJevDecision = [&](const JevDecision&) { order += 'J'; };

    auto* response = harness->responses.getNextToWriteTo();
    response->type_ = Exchange::ClientResponseType::ACCEPTED;
    response->ticker_id_ = 0;
    response->side_ = Common::Side::BUY;
    harness->responses.updateWriteIndex();

    writeMarketUpdate(harness->market_updates, 0, 100, 101);
    writeDecision(harness->decisions, 1);
    engine.registerEvaluation(0, 1);

    EXPECT_TRUE(engine.processPending());
    EXPECT_EQ("RMJ", order);
    EXPECT_EQ(100, engine.marketOrderBook(0).getBBO()->bid_price_);
}

TEST(SimulatedVenueFlowTest, OpenFillAccruesPositionAndCloseFreesIt) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    auto& engine = harness->engine;
    Trading::OrderGateway gateway(7, &harness->requests, &harness->responses);
    SimulatedVenue venue(&gateway);

    engine.orderManager().moveOrders(0, 100, Common::Price_INVALID, 2);
    ASSERT_EQ(1u, harness->requests.size());
    ASSERT_TRUE(gateway.processPending());
    ASSERT_TRUE(engine.processPending());
    auto& open_order = engine.orderManager().getOrder(0, Common::Side::BUY);
    ASSERT_EQ(OMOrderState::LIVE, open_order.order_state_);

    venue.fill(open_order.order_id_, 100, 2);
    ASSERT_TRUE(engine.processPending());
    const auto& opened = engine.positionKeeper().getPositionInfo(0);
    EXPECT_EQ(2, opened.position_);
    EXPECT_EQ(2u, opened.todayQty());

    engine.orderManager().moveCloseOrder(0, 101, Common::Side::SELL, 2);
    ASSERT_EQ(1u, harness->requests.size());
    const auto& close_order = engine.orderManager().getOrder(0, Common::Side::SELL);
    ASSERT_EQ(Common::OrderOffset::CLOSE_TODAY, close_order.offset_);
    gateway.processPending();
    engine.processPending();
    venue.fill(close_order.order_id_, 101, 2);
    engine.processPending();
    const auto& closed = engine.positionKeeper().getPositionInfo(0);
    EXPECT_EQ(0, closed.position_);
    EXPECT_EQ(0u, closed.todayQty());
    EXPECT_DOUBLE_EQ(2.0, closed.realized_pnl_);
}

TEST(QueueBoundaryTest, MarketDataConsumerPublishesIntoTheQueue) {
    Exchange::MarketUpdateLFQueue market_updates(8);
    Trading::MarketDataConsumer consumer(7, &market_updates);
    Exchange::MarketUpdate update;
    update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;

    consumer.publishMarketUpdate(update);

    EXPECT_EQ(1u, consumer.localReceiveSequence());
    EXPECT_EQ(1u, market_updates.size());
}

TEST(QueueBoundaryTest, OrderGatewayRoutesRequestsAndStampsSequences) {
    Exchange::ClientRequestLFQueue requests(8);
    Exchange::ClientResponseLFQueue responses(8);
    Trading::OrderGateway gateway(7, &requests, &responses);

    std::size_t seen_sequence = 0;
    gateway.setRequestHandler([&](std::size_t sequence, const Exchange::ClientRequest&) {
        seen_sequence = sequence;
    });
    auto* request = requests.getNextToWriteTo();
    request->client_id_ = 7;
    request->type_ = Exchange::ClientRequestType::CANCEL;
    requests.updateWriteIndex();

    EXPECT_TRUE(gateway.processPending());
    EXPECT_EQ(1u, seen_sequence);
    EXPECT_EQ(2u, gateway.nextOutgoingSequence());
}

TEST(EvaluationSchedulingTest, SchedulesEvaluationAndMapsDecisionToOrder) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    auto& engine = harness->engine;
    Trading::JevEvaluationStateLFQueue evaluations(32);
    MockJevDecisionProvider provider;
    JevWorker worker(&evaluations, &harness->decisions, &provider);

    writeMarketUpdate(harness->market_updates, 0, 99, 101);
    engine.processPending();

    engine.attachJevEvaluationQueue(&evaluations, kLongInterval, 0);
    EXPECT_TRUE(engine.processPending());
    ASSERT_EQ(1u, evaluations.size());

    EXPECT_TRUE(worker.processPending());
    EXPECT_TRUE(engine.processPending());
    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(7u, request.client_id_);
    EXPECT_EQ(Common::Side::SELL, request.side_);
    EXPECT_EQ(99, request.price_);
    EXPECT_EQ(Common::OrderOffset::OPEN, request.offset_);
}

TEST(FeatureSnapshotTest, PublishesValidSnapshotsAndRecoversAcrossVenueModes) {
    for (const int venue : {0, 1, 2}) {
        auto harness = makeHarness({10, 10, 0.0});
        auto& engine = harness->engine;
        if (venue == 1) engine.enableSimexConstraints();
        if (venue == 2) {
            engine.enableSingleOrderVenue();
            engine.updateVenueState(true, {}, 1);
        }
        Trading::JevEvaluationStateLFQueue evaluations(32);
        engine.attachJevEvaluationQueue(&evaluations, std::chrono::milliseconds(0));

        const auto publish = [&](Common::Price bid, Common::Price ask) {
            writeMarketUpdate(harness->market_updates, 0, bid, ask);
            engine.processPending();
        };

        publish(100, 101);
        ASSERT_EQ(1u, evaluations.size());
        const auto captured = *evaluations.getNextToRead();
        evaluations.updateReadIndex();

        publish(100, Common::Price_INVALID);
        EXPECT_EQ(0u, evaluations.size());
        EXPECT_FALSE(engine.acceptsEvaluation(captured));

        MockJevDecisionProvider provider;
        JevWorker worker(&evaluations, &harness->decisions, &provider);
        worker.setEvaluationFilter([&](const auto& state) { return engine.acceptsEvaluation(state); });
        writeEvaluation(evaluations, captured.evaluation_id_) = captured;
        evaluations.updateWriteIndex();
        EXPECT_TRUE(worker.processPending());
        EXPECT_EQ(0u, harness->decisions.size());

        publish(110, 112);
        ASSERT_EQ(1u, evaluations.size());
        EXPECT_EQ(captured.evaluation_id_ + 1, evaluations.getNextToRead()->evaluation_id_);
        EXPECT_EQ(111, evaluations.getNextToRead()->market_features_.mid_price_);
        evaluations.updateReadIndex();

        writeDecision(harness->decisions, captured.evaluation_id_);
        engine.processPending();
        EXPECT_EQ(0u, harness->requests.size());
    }
}

TEST(IncrementalFeatureTest, UpdatesFeaturesFromIncrementalMarketUpdates) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    auto& engine = harness->engine;
    Trading::JevEvaluationStateLFQueue evaluations(8);
    engine.attachJevEvaluationQueue(&evaluations, std::chrono::milliseconds(0));

    const auto publish = [&](Exchange::MarketUpdateType type, Common::OrderId id,
                             Common::Side side, Common::Price price, Common::Qty qty) {
        auto* update = harness->market_updates.getNextToWriteTo();
        update->type_ = type;
        update->ticker_id_ = 0;
        update->order_id_ = id;
        update->side_ = side;
        update->price_ = price;
        update->qty_ = qty;
        harness->market_updates.updateWriteIndex();
        engine.processPending();
    };

    publish(Exchange::MarketUpdateType::ADD, 1, Common::Side::BUY, 100, 2);
    EXPECT_EQ(0u, evaluations.size());
    publish(Exchange::MarketUpdateType::ADD, 2, Common::Side::SELL, 102, 6);
    ASSERT_EQ(1u, evaluations.size());
    EXPECT_EQ(101, evaluations.getNextToRead()->market_features_.mid_price_);
    evaluations.updateReadIndex();

    publish(Exchange::MarketUpdateType::MODIFY, 1, Common::Side::BUY, 100, 6);
    ASSERT_EQ(1u, evaluations.size());
    EXPECT_DOUBLE_EQ(0.0, evaluations.getNextToRead()->market_features_.top_of_book_qty_imbalance_);
    evaluations.updateReadIndex();

    publish(Exchange::MarketUpdateType::CLEAR, 0, Common::Side::INVALID, 0, 0);
    EXPECT_EQ(0u, evaluations.size());
}

TEST(SimexGuardTest, GatesEvaluationsAndOpenDecisionsOnReadinessAndPosition) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    auto& engine = harness->engine;
    engine.enableSimexConstraints();
    Trading::JevEvaluationStateLFQueue evaluations(16);
    engine.attachJevEvaluationQueue(&evaluations, kLongInterval);

    const auto publishDepth = [&](bool available) {
        auto* update = harness->market_updates.getNextToWriteTo();
        update->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
        update->ticker_id_ = 0;
        if (available) {
            update->depth_snapshot_.bids_[0] = {99, 10};
            update->depth_snapshot_.asks_[0] = {101, 10};
        }
        harness->market_updates.updateWriteIndex();
        engine.processPending();
    };
    const auto decide = [&](std::uint64_t id, JevBias bias) {
        writeDecision(harness->decisions, id, bias);
        engine.processPending();
    };

    publishDepth(false);
    EXPECT_EQ(0u, evaluations.size());

    publishDepth(true);
    ASSERT_EQ(1u, evaluations.size());
    const auto stale_id = evaluations.getNextToRead()->evaluation_id_;
    evaluations.updateReadIndex();

    publishDepth(false);
    publishDepth(true);
    decide(stale_id, JevBias::LONG);
    EXPECT_EQ(0u, harness->requests.size());

    engine.registerEvaluation(0, 100);
    decide(100, JevBias::LONG);
    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);

    engine.registerEvaluation(0, 101);
    decide(101, JevBias::SHORT);
    EXPECT_EQ(0u, harness->requests.size());

    writeFill(harness->responses, 7, request.order_id_, Common::Side::BUY, 101, 1);
    engine.processPending();
    EXPECT_EQ(1, engine.positionKeeper().getPositionInfo(0).position_);

    engine.registerEvaluation(0, 102);
    decide(102, JevBias::SHORT);
    EXPECT_EQ(0u, harness->requests.size());
}

TEST(DecisionExpiryTest, WorkerEvaluatesAndEngineDropsStaleDecisions) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    auto& engine = harness->engine;
    Trading::JevEvaluationStateLFQueue states(8);
    MockJevDecisionProvider provider;
    JevWorker worker(&states, &harness->decisions, &provider);

    writeMarketUpdate(harness->market_updates, 0, 99, 101);
    engine.processPending();

    writeEvaluation(states, 3);
    states.updateWriteIndex();
    EXPECT_TRUE(worker.processPending());
    engine.registerEvaluation(0, 3);

    std::size_t accepted = 0;
    engine.onJevDecision = [&](const JevDecision& decision) {
        EXPECT_EQ(3u, decision.evaluation_id_);
        ++accepted;
    };
    EXPECT_TRUE(engine.processPending());
    EXPECT_EQ(1u, accepted);

    writeDecision(harness->decisions, 2);
    EXPECT_TRUE(engine.processPending());
    EXPECT_EQ(1u, accepted);
}

TEST(WorkerFailureTest, CountsProviderFailures) {
    Trading::JevEvaluationStateLFQueue states(8);
    Trading::JevDecisionLFQueue decisions(8);
    FailingJevProvider provider;
    JevWorker worker(&states, &decisions, &provider);

    writeEvaluation(states, 1);
    states.updateWriteIndex();
    writeEvaluation(states, 2);
    states.updateWriteIndex();

    EXPECT_TRUE(worker.processPending());
    EXPECT_EQ(0u, states.size());
    EXPECT_EQ(0u, decisions.size());
    EXPECT_EQ(1u, worker.failureCount());
}

TEST(EvaluationSnapshotTest, CarriesPositionAndRiskIntoTheEvaluationState) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    auto& engine = harness->engine;
    writeMarketUpdate(harness->market_updates, 0, 99, 101);
    engine.processPending();

    const auto state = engine.buildJevEvaluationState(0, 5);
    EXPECT_EQ(5u, state.evaluation_id_);
    EXPECT_EQ(0u, state.ticker_id_);
    EXPECT_DOUBLE_EQ(1.0, state.price_scale_);
    EXPECT_EQ(10u, state.risk_.max_order_size_);
    EXPECT_EQ(10u, state.risk_.max_position_);
    EXPECT_DOUBLE_EQ(0.0, state.risk_.max_loss_);
    EXPECT_EQ(99, state.depth_snapshot_.bids_[0].price_);
    EXPECT_EQ(101, state.depth_snapshot_.asks_[0].price_);
    EXPECT_DOUBLE_EQ(100.0, state.market_features_.mid_price_);
    EXPECT_TRUE(state.market_features_.valid_);
}

}  // namespace
