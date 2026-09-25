#include <cassert>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/lf_queue.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/jev_decision.h"
#include "trading/strategy/jev_http_client.h"
#include "trading/strategy/jev_worker.h"
#include "trading/strategy/market_order_book.h"
#include "trading/strategy/om_order.h"
#include "trading/strategy/trade_engine.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"
#include "simulated_venue.h"

namespace {

void testQueueContract() {
  Common::LFQueue<int> queue(2);

  auto* first = queue.tryGetNextToWriteTo();
  assert(first != nullptr);
  *first = 11;
  queue.updateWriteIndex();

  auto* second = queue.tryGetNextToWriteTo();
  assert(second != nullptr);
  *second = 22;
  queue.updateWriteIndex();

  assert(queue.isFull());
  assert(queue.tryGetNextToWriteTo() == nullptr);

  const auto* read_first = queue.getNextToRead();
  assert(read_first != nullptr && *read_first == 11);
  queue.updateReadIndex();

  const auto* read_second = queue.getNextToRead();
  assert(read_second != nullptr && *read_second == 22);
  queue.updateReadIndex();
  assert(queue.size() == 0);
}

void testQueueSpscConcurrency() {
  constexpr int message_count = 10'000;
  Common::LFQueue<int> queue(64);
  std::atomic<bool> producer_done{false};
  std::atomic<int> consumed{0};

  std::thread producer([&] {
    for (int value = 0; value < message_count; ++value) {
      int* slot = nullptr;
      while ((slot = queue.tryGetNextToWriteTo()) == nullptr) {
        std::this_thread::yield();
      }
      *slot = value;
      queue.updateWriteIndex();
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    int expected = 0;
    while (!producer_done.load(std::memory_order_acquire) || queue.size() != 0) {
      const int* slot = queue.getNextToRead();
      if (slot == nullptr) {
        std::this_thread::yield();
        continue;
      }
      assert(*slot == expected);
      ++expected;
      consumed.fetch_add(1, std::memory_order_relaxed);
      queue.updateReadIndex();
    }
    assert(expected == message_count);
  });

  producer.join();
  consumer.join();
  assert(consumed.load(std::memory_order_relaxed) == message_count);
}

void testMessageContract() {
  Exchange::MarketUpdate snapshot;
  snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  snapshot.depth_snapshot_.last_price_ = 1234;
  snapshot.depth_snapshot_.bids_[0] = {1233, 10};
  snapshot.depth_snapshot_.asks_[0] = {1235, 12};
  assert(snapshot.toString().find("DEPTH_SNAPSHOT") != std::string::npos);

  Exchange::ClientRequest request;
  request.type_ = Exchange::ClientRequestType::NEW;
  request.order_type_ = Common::OrderType::LIMIT;
  request.offset_ = Common::OrderOffset::OPEN;
  assert(request.toString().find("LIMIT") != std::string::npos);

  Exchange::ClientResponse response;
  response.type_ = Exchange::ClientResponseType::REJECTED;
  assert(response.toString().find("REJECTED") != std::string::npos);

  Trading::OMOrder order;
  order.order_type_ = Common::OrderType::LIMIT;
  order.offset_ = Common::OrderOffset::CLOSE_YESTERDAY;
  order.order_state_ = Trading::OMOrderState::PENDING_NEW;
  assert(order.toString().find("CLOSE_YESTERDAY") != std::string::npos);
}

void testMarketOrderBookContract() {
  Trading::MarketOrderBook snapshot_book(0);
  Exchange::MarketUpdate snapshot;
  snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  snapshot.depth_snapshot_.bids_[0] = {1233, 10};
  snapshot.depth_snapshot_.asks_[0] = {1235, 12};
  snapshot_book.onMarketUpdate(snapshot);

  assert(snapshot_book.getBBO()->bid_price_ == 1233);
  assert(snapshot_book.getBBO()->bid_qty_ == 10);
  assert(snapshot_book.getBBO()->ask_price_ == 1235);
  assert(snapshot_book.getBBO()->ask_qty_ == 12);

  Trading::MarketOrderBook incremental_book(0);
  Exchange::MarketUpdate add;
  add.type_ = Exchange::MarketUpdateType::ADD;
  add.order_id_ = 1;
  add.side_ = Common::Side::BUY;
  add.price_ = 1234;
  add.qty_ = 4;
  incremental_book.onMarketUpdate(add);
  assert(incremental_book.getBBO()->bid_price_ == 1234);
  assert(incremental_book.getBBO()->bid_qty_ == 4);

  add.type_ = Exchange::MarketUpdateType::CANCEL;
  incremental_book.onMarketUpdate(add);
  assert(incremental_book.getBBO()->bid_price_ == Common::Price_INVALID);
}

void testJevDecisionContract() {
  Trading::JevDecisionLFQueue queue(4);
  auto* slot = queue.getNextToWriteTo();
  slot->evaluation_id_ = 7;
  slot->ticker_id_ = 0;
  slot->bias_ = Trading::JevBias::LONG;
  slot->intent_ = Trading::JevIntent::OPEN;
  queue.updateWriteIndex();

  const auto* decision = queue.getNextToRead();
  assert(decision != nullptr);
  assert(decision->evaluation_id_ == 7);
  assert(decision->bias_ == Trading::JevBias::LONG);
  assert(decision->intent_ == Trading::JevIntent::OPEN);
  queue.updateReadIndex();
}

void testTradeEngineDispatchOrder() {
  Exchange::ClientRequestLFQueue requests(8);
  Exchange::ClientResponseLFQueue responses(8);
  Exchange::MarketUpdateLFQueue market_updates(8);
  Trading::JevDecisionLFQueue decisions(8);
  Trading::TradeEngine engine(
      7, Trading::RiskLimits{10, 100, 0.0}, &requests, &responses,
      &market_updates, &decisions);

  std::string order;
  engine.onClientResponse = [&](const Exchange::ClientResponse&) { order += 'R'; };
  engine.onMarketUpdate = [&](const Exchange::MarketUpdate&, const Trading::MarketOrderBook&) { order += 'M'; };
  engine.onJevDecision = [&](const Trading::JevDecision&) { order += 'J'; };

  auto* response = responses.getNextToWriteTo();
  response->type_ = Exchange::ClientResponseType::ACCEPTED;
  response->ticker_id_ = 0;
  response->side_ = Common::Side::BUY;
  responses.updateWriteIndex();

  auto* update = market_updates.getNextToWriteTo();
  update->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  update->ticker_id_ = 0;
  update->depth_snapshot_.bids_[0] = {100, 2};
  update->depth_snapshot_.asks_[0] = {101, 3};
  market_updates.updateWriteIndex();

  auto* decision = decisions.getNextToWriteTo();
  decision->evaluation_id_ = 1;
  decision->ticker_id_ = 0;
  decision->bias_ = Trading::JevBias::LONG;
  decision->intent_ = Trading::JevIntent::OPEN;
  decisions.updateWriteIndex();
  engine.registerEvaluation(0, 1);

  assert(engine.processPending());
  assert(order == "RMJ");
  assert(engine.marketOrderBook(0).getBBO()->bid_price_ == 100);
}

void testBookModeProtection() {
  Trading::MarketOrderBook book(0);
  Exchange::MarketUpdate snapshot;
  snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  snapshot.depth_snapshot_.bids_[0] = {100, 1};
  snapshot.depth_snapshot_.asks_[0] = {101, 1};
  book.onMarketUpdate(snapshot);

  Exchange::MarketUpdate add;
  add.type_ = Exchange::MarketUpdateType::ADD;
  add.order_id_ = 1;
  add.side_ = Common::Side::BUY;
  add.price_ = 99;
  add.qty_ = 1;
  bool threw = false;
  try {
    book.onMarketUpdate(add);
  } catch (const std::logic_error&) {
    threw = true;
  }
  assert(threw);
}

void testSimulatedVenueTradingPath() {
  Exchange::ClientRequestLFQueue requests(32);
  Exchange::ClientResponseLFQueue responses(32);
  Exchange::MarketUpdateLFQueue market_updates(32);
  Trading::JevDecisionLFQueue decisions(32);
  Trading::TradeEngine engine(
      7, Trading::RiskLimits{10, 10, 0.0}, &requests, &responses,
      &market_updates, &decisions);
  Trading::OrderGateway gateway(7, &requests, &responses);
  Tests::SimulatedVenue venue(&gateway);

  engine.orderManager().moveOrders(0, 100, Common::Price_INVALID, 2);
  assert(requests.size() == 1);
  assert(gateway.processPending());
  assert(responses.size() == 1);
  assert(engine.processPending());
  auto& open_order = engine.orderManager().getOrder(0, Common::Side::BUY);
  assert(open_order.order_state_ == Trading::OMOrderState::LIVE);

  venue.fill(open_order.order_id_, 100, 2);
  assert(engine.processPending());
  const auto& opened = engine.positionKeeper().getPositionInfo(0);
  assert(opened.position_ == 2);
  assert(opened.todayQty() == 2);

  engine.orderManager().moveCloseOrder(0, 101, Common::Side::SELL, 2);
  assert(requests.size() == 1);
  const auto& close_order = engine.orderManager().getOrder(0, Common::Side::SELL);
  assert(close_order.offset_ == Common::OrderOffset::CLOSE_TODAY);
  gateway.processPending();
  engine.processPending();
  venue.fill(close_order.order_id_, 101, 2);
  engine.processPending();
  const auto& closed = engine.positionKeeper().getPositionInfo(0);
  assert(closed.position_ == 0);
  assert(closed.todayQty() == 0);
  assert(closed.realized_pnl_ == 2.0);
}

void testQueueFacingAdapters() {
  Exchange::MarketUpdateLFQueue market_updates(8);
  Trading::MarketDataConsumer consumer(7, &market_updates);
  consumer.start();
  Exchange::MarketUpdate update;
  update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  consumer.publishMarketUpdate(update);
  assert(consumer.localReceiveSequence() == 1);
  assert(market_updates.size() == 1);
  consumer.stop();

  Exchange::ClientRequestLFQueue requests(8);
  Exchange::ClientResponseLFQueue responses(8);
  Trading::OrderGateway gateway(7, &requests, &responses);
  std::size_t seen_sequence = 0;
  gateway.setRequestHandler(
      [&](std::size_t sequence, const Exchange::ClientRequest&) {
        seen_sequence = sequence;
      });
  auto* request = requests.getNextToWriteTo();
  request->client_id_ = 7;
  request->type_ = Exchange::ClientRequestType::CANCEL;
  requests.updateWriteIndex();
  assert(gateway.processPending());
  assert(seen_sequence == 1);
  assert(gateway.nextOutgoingSequence() == 2);
}

void testJevWorkerAndEvaluationExpiry() {
  Exchange::ClientRequestLFQueue requests(8);
  Exchange::ClientResponseLFQueue responses(8);
  Exchange::MarketUpdateLFQueue market_updates(8);
  Trading::JevDecisionLFQueue decisions(8);
  Trading::JevEvaluationStateLFQueue states(8);
  Trading::TradeEngine engine(
      7, Trading::RiskLimits{10, 10, 0.0}, &requests, &responses,
      &market_updates, &decisions);
  Trading::MockJevDecisionProvider provider;
  Trading::JevWorker worker(&states, &decisions, &provider);

  auto* state = states.getNextToWriteTo();
  state->evaluation_id_ = 3;
  state->ticker_id_ = 0;
  states.updateWriteIndex();
  assert(worker.processPending());
  engine.registerEvaluation(0, 3);
  std::size_t accepted = 0;
  engine.onJevDecision = [&](const Trading::JevDecision& decision) {
    assert(decision.evaluation_id_ == 3);
    ++accepted;
  };
  assert(engine.processPending());
  assert(accepted == 1);

  auto* stale = decisions.getNextToWriteTo();
  stale->evaluation_id_ = 2;
  stale->ticker_id_ = 0;
  decisions.updateWriteIndex();
  assert(engine.processPending());
  assert(accepted == 1);
}

void testJevHttpRequestContract() {
  Trading::JevHttpClient client(Trading::JevHttpConfig{});
  Trading::JevEvaluationState state;
  state.evaluation_id_ = 9;
  state.ticker_id_ = 0;
  state.position_.net_position_ = 0;
  state.depth_snapshot_.last_price_ = 100;
  state.depth_snapshot_.bids_[0] = {99, 2};
  state.depth_snapshot_.asks_[0] = {101, 3};
  state.working_orders_[0].order_id_ = 12;
  state.working_orders_[0].side_ = Common::Side::BUY;
  state.working_orders_[0].state_ = Trading::OMOrderState::LIVE;

  const auto body = client.buildRequestBody(state);
  assert(body.find("\"model\":\"jev-latest\"") != std::string::npos);
  assert(body.find("\"evaluation_id\": 9") != std::string::npos);
  assert(body.find("\"working_orders\"") != std::string::npos);
  assert(body.find("\"order_id\": 12") != std::string::npos);
  assert(body.find("\"open\":\"open a position\"") != std::string::npos);
  assert(body.find("\"hold\":\"take no action\"") != std::string::npos);
  assert(body.find("\"close\":\"reduce the current position\"") ==
         std::string::npos);

  state.position_.net_position_ = 2;
  const auto occupied_body = client.buildRequestBody(state);
  assert(occupied_body.find("\"close\":\"reduce the current position\"") !=
         std::string::npos);
}

void testJevEvaluationSchedulingAndOrderMapping() {
  Exchange::ClientRequestLFQueue requests(16);
  Exchange::ClientResponseLFQueue responses(16);
  Exchange::MarketUpdateLFQueue market_updates(16);
  Trading::JevDecisionLFQueue decisions(16);
  Trading::JevEvaluationStateLFQueue evaluations(16);
  Trading::TradeEngine engine(
      7, Trading::RiskLimits{10, 10, 0.0}, &requests, &responses,
      &market_updates, &decisions);
  Trading::MockJevDecisionProvider provider;
  Trading::JevWorker worker(&evaluations, &decisions, &provider);

  auto* market = market_updates.getNextToWriteTo();
  market->type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  market->ticker_id_ = 0;
  market->depth_snapshot_.bids_[0] = {99, 4};
  market->depth_snapshot_.asks_[0] = {101, 5};
  market_updates.updateWriteIndex();
  engine.processPending();

  engine.attachJevEvaluationQueue(
      &evaluations, std::chrono::hours(1), 0);
  assert(engine.processPending());
  assert(evaluations.size() == 1);

  assert(worker.processPending());
  assert(engine.processPending());
  assert(requests.size() == 1);
  const auto* request = requests.getNextToRead();
  assert(request != nullptr);
  assert(request->client_id_ == 7);
  assert(request->side_ == Common::Side::SELL);
  assert(request->price_ == 99);
  assert(request->offset_ == Common::OrderOffset::OPEN);
}

void testYesterdayFirstPositionClose() {
  Trading::PositionInfo position;
  position.position_ = 3;
  position.position_days_[static_cast<std::size_t>(Common::PositionDay::YESTERDAY)] =
      {2, 95.0};
  position.position_days_[static_cast<std::size_t>(Common::PositionDay::TODAY)] =
      {1, 100.0};

  assert(position.closeOffsetFor(1) == Common::OrderOffset::CLOSE_YESTERDAY);

  Exchange::ClientResponse response;
  response.type_ = Exchange::ClientResponseType::FILLED;
  response.ticker_id_ = 0;
  response.side_ = Common::Side::SELL;
  response.price_ = 97;
  response.exec_qty_ = 2;
  response.leaves_qty_ = 0;
  response.offset_ = Common::OrderOffset::CLOSE_YESTERDAY;
  position.onFill(response);

  assert(position.position_ == 1);
  assert(position.yesterdayQty() == 0);
  assert(position.todayQty() == 1);
  assert(position.realized_pnl_ == 4.0);
}

}  // namespace

int main() {
  testQueueContract();
  testQueueSpscConcurrency();
  testMessageContract();
  testMarketOrderBookContract();
  testJevDecisionContract();
  testTradeEngineDispatchOrder();
  testBookModeProtection();
  testSimulatedVenueTradingPath();
  testQueueFacingAdapters();
  testJevWorkerAndEvaluationExpiry();
  testJevHttpRequestContract();
  testJevEvaluationSchedulingAndOrderMapping();
  testYesterdayFirstPositionClose();
  return 0;
}
