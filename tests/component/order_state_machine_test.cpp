#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "harness.h"

namespace {

using Tests::EngineHarness;
using Tests::makeHarness;
using Tests::readRequest;
using Tests::writeResponse;

using Trading::OMOrderState;

auto placeBuyOrder(EngineHarness& harness, Common::Price price, Common::Qty qty)
        -> Exchange::ClientRequest {
    harness.engine.orderManager().moveOpenOrder(0, price, Common::Side::BUY, qty);
    return readRequest(harness.requests);
}

auto acceptOrder(EngineHarness& harness, Common::OrderId order_id, Common::Side side) -> void {
    writeResponse(harness.responses, Exchange::ClientResponseType::ACCEPTED, order_id, side);
    harness.engine.processPending();
}

auto mutablePosition(EngineHarness& harness) -> Trading::PositionInfo& {
    return const_cast<Trading::PositionInfo&>(
            harness.engine.positionKeeper().getPositionInfo(0));
}

auto seedPositionDay(EngineHarness& harness, Common::PositionDay day, Common::Qty qty,
                     Common::Price open_vwap) -> void {
    auto& day_info =
            mutablePosition(harness).position_days_[static_cast<std::size_t>(day)];
    day_info.qty_ = qty;
    day_info.open_vwap_ = static_cast<double>(open_vwap);
}

TEST(OrderStateMachineTest, NewOpenOrderSendsNewRequestAndEntersPendingNew) {
    auto harness = makeHarness(EngineHarness::defaultLimits());

    const auto request = placeBuyOrder(*harness, 100, 2);

    EXPECT_EQ(Exchange::ClientRequestType::NEW, request.type_);
    EXPECT_EQ(7u, request.client_id_);
    EXPECT_EQ(0u, request.ticker_id_);
    EXPECT_EQ(1u, request.order_id_);
    EXPECT_EQ(Common::Side::BUY, request.side_);
    EXPECT_EQ(100, request.price_);
    EXPECT_EQ(2u, request.qty_);
    EXPECT_EQ(Common::OrderType::LIMIT, request.order_type_);
    EXPECT_EQ(Common::OrderOffset::OPEN, request.offset_);

    const auto& order = harness->engine.orderManager().getOrder(0, Common::Side::BUY);
    EXPECT_EQ(1u, order.order_id_);
    EXPECT_EQ(100, order.price_);
    EXPECT_EQ(2u, order.qty_);
    EXPECT_EQ(OMOrderState::PENDING_NEW, order.order_state_);
}

TEST(OrderStateMachineTest, AcceptedResponsePromotesPendingNewOrderToLive) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);

    acceptOrder(*harness, 1, Common::Side::BUY);

    EXPECT_EQ(OMOrderState::LIVE,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
    EXPECT_EQ(0u, harness->requests.size());
}

TEST(OrderStateMachineTest, CanceledResponseKillsLiveOrder) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);

    writeResponse(harness->responses, Exchange::ClientResponseType::CANCELED, 1,
                  Common::Side::BUY);
    harness->engine.processPending();

    EXPECT_EQ(OMOrderState::DEAD,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, RejectedResponseKillsPendingNewOrder) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);

    writeResponse(harness->responses, Exchange::ClientResponseType::REJECTED, 1,
                  Common::Side::BUY);
    harness->engine.processPending();

    EXPECT_EQ(OMOrderState::DEAD,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, PartialFillKeepsLiveOrderWithReducedQuantity) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);

    writeResponse(harness->responses, Exchange::ClientResponseType::FILLED, 1,
                  Common::Side::BUY, 100, 1, 1);
    harness->engine.processPending();

    const auto& order = harness->engine.orderManager().getOrder(0, Common::Side::BUY);
    EXPECT_EQ(OMOrderState::LIVE, order.order_state_);
    EXPECT_EQ(1u, order.qty_);
}

TEST(OrderStateMachineTest, FullFillKillsTheOrder) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);

    writeResponse(harness->responses, Exchange::ClientResponseType::FILLED, 1,
                  Common::Side::BUY, 100, 2, 0);
    harness->engine.processPending();

    EXPECT_EQ(OMOrderState::DEAD,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, CancelRejectedReturnsPendingCancelOrderToLive) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);

    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);
    ASSERT_EQ(1u, harness->requests.size());
    const auto cancel = readRequest(harness->requests);
    EXPECT_EQ(Exchange::ClientRequestType::CANCEL, cancel.type_);
    EXPECT_EQ(1u, cancel.order_id_);
    EXPECT_EQ(100, cancel.price_);
    EXPECT_EQ(OMOrderState::PENDING_CANCEL,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);

    writeResponse(harness->responses, Exchange::ClientResponseType::CANCEL_REJECTED, 1,
                  Common::Side::BUY);
    harness->engine.processPending();

    const auto& order = harness->engine.orderManager().getOrder(0, Common::Side::BUY);
    EXPECT_EQ(OMOrderState::LIVE, order.order_state_);
    EXPECT_EQ(100, order.price_);
}

TEST(OrderStateMachineTest, MatchingMoveOnLiveOrderSendsNothing) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);

    harness->engine.orderManager().moveOpenOrder(0, 100, Common::Side::BUY, 2);

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(OMOrderState::LIVE,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, RepricingMoveCancelsLiveOrderKeepingItsDetails) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);

    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);

    ASSERT_EQ(1u, harness->requests.size());
    const auto cancel = readRequest(harness->requests);
    EXPECT_EQ(Exchange::ClientRequestType::CANCEL, cancel.type_);
    EXPECT_EQ(1u, cancel.order_id_);
    EXPECT_EQ(100, cancel.price_);
    const auto& order = harness->engine.orderManager().getOrder(0, Common::Side::BUY);
    EXPECT_EQ(OMOrderState::PENDING_CANCEL, order.order_state_);
    EXPECT_EQ(100, order.price_);
}

TEST(OrderStateMachineTest, MoveWhilePendingNewIsIgnored) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);

    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(OMOrderState::PENDING_NEW,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, MoveWhilePendingCancelIsIgnored) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);
    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);
    readRequest(harness->requests);

    harness->engine.orderManager().moveOpenOrder(0, 103, Common::Side::BUY, 2);

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(OMOrderState::PENDING_CANCEL,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, ReplacementOrderFollowsCompletedCancel) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);
    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);
    readRequest(harness->requests);
    writeResponse(harness->responses, Exchange::ClientResponseType::CANCELED, 1,
                  Common::Side::BUY);
    harness->engine.processPending();
    ASSERT_EQ(OMOrderState::DEAD,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);

    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);

    ASSERT_EQ(1u, harness->requests.size());
    const auto replacement = readRequest(harness->requests);
    EXPECT_EQ(Exchange::ClientRequestType::NEW, replacement.type_);
    EXPECT_EQ(2u, replacement.order_id_);
    EXPECT_EQ(102, replacement.price_);
    EXPECT_EQ(OMOrderState::PENDING_NEW,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, LateCancelRejectionLeavesNewerOrderUntouched) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    placeBuyOrder(*harness, 100, 2);
    acceptOrder(*harness, 1, Common::Side::BUY);
    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);
    readRequest(harness->requests);
    writeResponse(harness->responses, Exchange::ClientResponseType::CANCELED, 1,
                  Common::Side::BUY);
    harness->engine.processPending();
    harness->engine.orderManager().moveOpenOrder(0, 102, Common::Side::BUY, 2);
    readRequest(harness->requests);
    ASSERT_EQ(OMOrderState::PENDING_NEW,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);

    writeResponse(harness->responses, Exchange::ClientResponseType::CANCEL_REJECTED, 1,
                  Common::Side::BUY);
    harness->engine.processPending();

    const auto& order = harness->engine.orderManager().getOrder(0, Common::Side::BUY);
    EXPECT_EQ(2u, order.order_id_);
    EXPECT_EQ(OMOrderState::PENDING_NEW, order.order_state_);
}

TEST(OrderStateMachineTest, InvalidMoveArgumentsPlaceNoOrder) {
    auto harness = makeHarness(EngineHarness::defaultLimits());

    harness->engine.orderManager().moveOpenOrder(0, Common::Price_INVALID, Common::Side::BUY, 2);
    harness->engine.orderManager().moveOpenOrder(0, 100, Common::Side::BUY, 0);

    EXPECT_EQ(0u, harness->requests.size());
    EXPECT_EQ(OMOrderState::INVALID,
              harness->engine.orderManager().getOrder(0, Common::Side::BUY).order_state_);
}

TEST(OrderStateMachineTest, CloseOrderPrefersYesterdayQuantity) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    seedPositionDay(*harness, Common::PositionDay::TODAY, 3, 101);
    seedPositionDay(*harness, Common::PositionDay::YESTERDAY, 2, 100);

    harness->engine.orderManager().moveCloseOrder(0, 99, Common::Side::SELL, 2);

    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Exchange::ClientRequestType::NEW, request.type_);
    EXPECT_EQ(Common::Side::SELL, request.side_);
    EXPECT_EQ(99, request.price_);
    EXPECT_EQ(2u, request.qty_);
    EXPECT_EQ(Common::OrderOffset::CLOSE_YESTERDAY, request.offset_);
}

TEST(OrderStateMachineTest, CloseOrderFallsBackToTodayQuantity) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    seedPositionDay(*harness, Common::PositionDay::TODAY, 2, 101);

    harness->engine.orderManager().moveCloseOrder(0, 99, Common::Side::SELL, 2);

    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(Common::OrderOffset::CLOSE_TODAY, request.offset_);
    EXPECT_EQ(2u, request.qty_);
}

TEST(OrderStateMachineTest, CloseOrderQuantityIsClampedToCloseableQuantity) {
    auto harness = makeHarness(EngineHarness::defaultLimits());
    seedPositionDay(*harness, Common::PositionDay::TODAY, 1, 101);

    harness->engine.orderManager().moveCloseOrder(0, 99, Common::Side::SELL, 5);

    ASSERT_EQ(1u, harness->requests.size());
    const auto request = readRequest(harness->requests);
    EXPECT_EQ(1u, request.qty_);
    EXPECT_EQ(Common::OrderOffset::CLOSE_TODAY, request.offset_);
}

TEST(OrderStateMachineTest, CloseOrderWithoutPositionIsSkipped) {
    auto harness = makeHarness(EngineHarness::defaultLimits());

    harness->engine.orderManager().moveCloseOrder(0, 99, Common::Side::SELL, 2);

    EXPECT_EQ(0u, harness->requests.size());
}

TEST(OrderStateMachineTest, CloseOrderBlockedByLossLimitIsSkipped) {
    auto harness = makeHarness({10, 10, 1.0});
    auto& position = mutablePosition(*harness);
    position.position_ = 2;
    seedPositionDay(*harness, Common::PositionDay::TODAY, 2, 100);
    writeResponse(harness->responses, Exchange::ClientResponseType::FILLED, 999,
                  Common::Side::SELL, 50, 1, 0, Common::OrderOffset::CLOSE_TODAY);
    harness->engine.processPending();
    ASSERT_EQ(1, position.position_);
    ASSERT_EQ(1u, position.todayQty());

    harness->engine.orderManager().moveCloseOrder(0, 99, Common::Side::SELL, 1);

    EXPECT_EQ(0u, harness->requests.size());
}

}  // namespace
