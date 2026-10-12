#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "harness.h"

namespace {

using Tests::EngineHarness;
using Tests::makeHarness;
using Tests::readRequest;
using Tests::writeDecision;
using Tests::writeFill;
using Tests::writeMarketUpdate;
using Tests::writeResponse;

using Trading::JevBias;
using Trading::JevIntent;

auto publishBook(EngineHarness& harness, Common::Price bid, Common::Price ask) -> void {
    writeMarketUpdate(harness.market_updates, 0, bid, ask);
    harness.engine.processPending();
}

auto publishOneSidedBook(EngineHarness& harness) -> void {
    auto* update = harness.market_updates.getNextToWriteTo();
    update->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update->ticker_id_ = 0;
    update->depth_snapshot_.bids_[0] = {99, 10};
    harness.market_updates.updateWriteIndex();
    harness.engine.processPending();
}

TEST(TradeEngineDecisionMatrixTest, UnknownEvaluationIdDecisionIsDropped) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    publishBook(*harness, 99, 101);

    writeDecision(harness->decisions, 7);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(0u, accepted);
}

TEST(TradeEngineDecisionMatrixTest, ZeroEvaluationIdDecisionIsDropped) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    publishBook(*harness, 99, 101);

    writeDecision(harness->decisions, 0);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(0u, accepted);
}

TEST(TradeEngineDecisionMatrixTest, StaleEvaluationIdDecisionIsDropped) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 3);
    writeDecision(harness->decisions, 3);
    harness->engine.processPending();
    ASSERT_EQ(1u, accepted);
    readRequest(harness->requests);

    harness->engine.registerEvaluation(0, 4);
    writeDecision(harness->decisions, 3);
    harness->engine.processPending();

    EXPECT_EQ(1u, accepted);
    EXPECT_EQ(0u, harness->requests.size());
}

TEST(TradeEngineDecisionMatrixTest, OutOfRangeTickerDecisionIsDropped) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 3);

    writeDecision(harness->decisions, 3, JevBias::LONG, JevIntent::OPEN, 9);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(0u, accepted);
}

TEST(TradeEngineDecisionMatrixTest, DecisionWithoutValidFeaturesIsDropped) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    publishOneSidedBook(*harness);
    harness->engine.registerEvaluation(0, 6);

    writeDecision(harness->decisions, 6);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(0u, accepted);
}

TEST(TradeEngineDecisionMatrixTest, DecisionWhileVenueNotReadyIsDropped) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    harness->engine.enableSingleOrderVenue();
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 8);

    writeDecision(harness->decisions, 8);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(0u, accepted);
}

TEST(TradeEngineDecisionMatrixTest, DecisionWithWorkingOrderIsDroppedUntilVenueIsFlat) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    harness->engine.enableSingleOrderVenue();
    harness->engine.updateVenueState(true, {}, 1);
    publishBook(*harness, 100, 101);

    harness->engine.orderManager().moveOrders(0, 100, 101, 2);
    ASSERT_EQ(2u, harness->requests.size());
    writeResponse(harness->responses, Exchange::ClientResponseType::ACCEPTED, 1,
                  Common::Side::BUY);
    writeResponse(harness->responses, Exchange::ClientResponseType::ACCEPTED, 2,
                  Common::Side::SELL);
    harness->engine.registerEvaluation(0, 9);
    writeDecision(harness->decisions, 9);
    harness->engine.processPending();

    EXPECT_EQ(0u, accepted);

    writeResponse(harness->responses, Exchange::ClientResponseType::CANCELED, 1,
                  Common::Side::BUY);
    writeResponse(harness->responses, Exchange::ClientResponseType::CANCELED, 2,
                  Common::Side::SELL);
    writeDecision(harness->decisions, 9);
    harness->engine.processPending();

    EXPECT_EQ(1u, accepted);
    ASSERT_EQ(3u, harness->requests.size());
    readRequest(harness->requests);
    readRequest(harness->requests);
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Common::Side::BUY, request.side_);
    EXPECT_EQ(101, request.price_);
}

TEST(TradeEngineDecisionMatrixTest, OpenDecisionIntoOppositePositionIsBlocked) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    harness->engine.enableSimexConstraints();
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 11);
    writeFill(harness->responses, 7, 999, Common::Side::BUY, 101, 1);
    harness->engine.processPending();
    ASSERT_EQ(1, harness->engine.positionKeeper().getPositionInfo(0).position_);

    writeDecision(harness->decisions, 11, JevBias::SHORT, JevIntent::OPEN);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());

    harness->engine.registerEvaluation(0, 12);
    writeDecision(harness->decisions, 12, JevBias::LONG, JevIntent::OPEN);
    harness->engine.processPending();

    EXPECT_EQ(2u, accepted);
    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Common::Side::BUY, request.side_);
}

TEST(TradeEngineDecisionMatrixTest, OpenDecisionAgainstOutstandingOppositeOrderIsBlocked) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    harness->engine.enableSimexConstraints();
    publishBook(*harness, 99, 101);

    harness->engine.orderManager().moveOrders(0, 99, 101, 1);
    ASSERT_EQ(2u, harness->requests.size());
    readRequest(harness->requests);
    readRequest(harness->requests);
    harness->engine.registerEvaluation(0, 13);

    writeDecision(harness->decisions, 13, JevBias::LONG, JevIntent::OPEN);
    harness->engine.processPending();
    EXPECT_EQ(0u, harness->requests.size());

    writeDecision(harness->decisions, 13, JevBias::SHORT, JevIntent::OPEN);
    harness->engine.processPending();
    EXPECT_EQ(0u, harness->requests.size());

    writeResponse(harness->responses, Exchange::ClientResponseType::CANCELED, 1,
                  Common::Side::BUY);
    writeResponse(harness->responses, Exchange::ClientResponseType::CANCELED, 2,
                  Common::Side::SELL);
    writeDecision(harness->decisions, 13, JevBias::SHORT, JevIntent::OPEN);
    harness->engine.processPending();

    EXPECT_EQ(3u, accepted);
    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Common::Side::SELL, request.side_);
    EXPECT_EQ(99, request.price_);
}

TEST(TradeEngineDecisionMatrixTest, OpenLongDecisionBuysAtTheAsk) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 1);

    writeDecision(harness->decisions, 1, JevBias::LONG, JevIntent::OPEN);
    harness->engine.processPending();

    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Exchange::ClientRequestType::NEW, request.type_);
    EXPECT_EQ(7u, request.client_id_);
    EXPECT_EQ(0u, request.ticker_id_);
    EXPECT_EQ(Common::Side::BUY, request.side_);
    EXPECT_EQ(101, request.price_);
    EXPECT_EQ(1u, request.qty_);
    EXPECT_EQ(Common::OrderType::LIMIT, request.order_type_);
    EXPECT_EQ(Common::OrderOffset::OPEN, request.offset_);
}

TEST(TradeEngineDecisionMatrixTest, OpenShortDecisionSellsAtTheBid) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 1);

    writeDecision(harness->decisions, 1, JevBias::SHORT, JevIntent::OPEN);
    harness->engine.processPending();

    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Common::Side::SELL, request.side_);
    EXPECT_EQ(99, request.price_);
    EXPECT_EQ(Common::OrderOffset::OPEN, request.offset_);
}

TEST(TradeEngineDecisionMatrixTest, ConfiguredQuantityCarriesIntoOpenOrder) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    harness->engine.setDecisionOrderQuantity(3);
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 1);

    writeDecision(harness->decisions, 1, JevBias::LONG, JevIntent::OPEN);
    harness->engine.processPending();

    ASSERT_EQ(1u, harness->requests.size());
    EXPECT_EQ(3u, readRequest(harness->requests).qty_);
}

TEST(TradeEngineDecisionMatrixTest, CloseLongDecisionSellsAtTheBid) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 21);
    writeFill(harness->responses, 7, 999, Common::Side::BUY, 101, 2);

    writeDecision(harness->decisions, 21, JevBias::LONG, JevIntent::CLOSE);
    harness->engine.processPending();

    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Common::Side::SELL, request.side_);
    EXPECT_EQ(99, request.price_);
    EXPECT_EQ(1u, request.qty_);
    EXPECT_EQ(Common::OrderOffset::CLOSE_TODAY, request.offset_);
}

TEST(TradeEngineDecisionMatrixTest, CloseShortDecisionBuysAtTheAsk) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 21);
    writeFill(harness->responses, 7, 999, Common::Side::SELL, 99, 2);

    writeDecision(harness->decisions, 21, JevBias::LONG, JevIntent::CLOSE);
    harness->engine.processPending();

    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Common::Side::BUY, request.side_);
    EXPECT_EQ(101, request.price_);
    EXPECT_EQ(Common::OrderOffset::CLOSE_TODAY, request.offset_);
}

TEST(TradeEngineDecisionMatrixTest, CloseDecisionWithFlatPositionIsANoOp) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 22);

    writeDecision(harness->decisions, 22, JevBias::LONG, JevIntent::CLOSE);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(1u, accepted);
}

TEST(TradeEngineDecisionMatrixTest, HoldDecisionPlacesNoOrder) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    std::size_t accepted = 0;
    harness->engine.onJevDecision = [&](const Trading::JevDecision&) { ++accepted; };
    publishBook(*harness, 99, 101);
    harness->engine.registerEvaluation(0, 2);

    writeDecision(harness->decisions, 2, JevBias::LONG, JevIntent::HOLD);
    harness->engine.processPending();

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(1u, accepted);
}

}  // namespace
