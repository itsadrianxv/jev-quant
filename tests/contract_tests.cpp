#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
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

[[noreturn]] void checkFailure(const char* condition, const char* file,
                                int line) {
  std::cerr << "check failed: " << condition << " (" << file << ":" << line
            << ")\n";
  std::abort();
}

#define CHECK(condition)                                                    \
  ((condition) ? static_cast<void>(0)                                       \
               : checkFailure(#condition, __FILE__, __LINE__))

void testQueueContract() {
  Common::LFQueue<int> queue(2);

  auto* first = queue.tryGetNextToWriteTo();
  CHECK(first != nullptr);
  *first = 11;
  queue.updateWriteIndex();

  auto* second = queue.tryGetNextToWriteTo();
  CHECK(second != nullptr);
  *second = 22;
  queue.updateWriteIndex();

  CHECK(queue.isFull());
  CHECK(queue.tryGetNextToWriteTo() == nullptr);

  const auto* read_first = queue.getNextToRead();
  CHECK(read_first != nullptr && *read_first == 11);
  queue.updateReadIndex();

  const auto* read_second = queue.getNextToRead();
  CHECK(read_second != nullptr && *read_second == 22);
  queue.updateReadIndex();
  CHECK(queue.size() == 0);
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
      CHECK(*slot == expected);
      ++expected;
      consumed.fetch_add(1, std::memory_order_relaxed);
      queue.updateReadIndex();
    }
    CHECK(expected == message_count);
  });

  producer.join();
  consumer.join();
  CHECK(consumed.load(std::memory_order_relaxed) == message_count);
}

void testMessageContract() {
  Exchange::MarketUpdate snapshot;
  snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  snapshot.depth_snapshot_.last_price_ = 1234;
  snapshot.depth_snapshot_.bids_[0] = {1233, 10};
  snapshot.depth_snapshot_.asks_[0] = {1235, 12};
  CHECK(snapshot.toString().find("DEPTH_SNAPSHOT") != std::string::npos);

  Exchange::ClientRequest request;
  request.type_ = Exchange::ClientRequestType::NEW;
  request.order_type_ = Common::OrderType::LIMIT;
  request.offset_ = Common::OrderOffset::OPEN;
  CHECK(request.toString().find("LIMIT") != std::string::npos);

  Exchange::ClientResponse response;
  response.type_ = Exchange::ClientResponseType::REJECTED;
  CHECK(response.toString().find("REJECTED") != std::string::npos);

  Trading::OMOrder order;
  order.order_type_ = Common::OrderType::LIMIT;
  order.offset_ = Common::OrderOffset::CLOSE_YESTERDAY;
  order.order_state_ = Trading::OMOrderState::PENDING_NEW;
  CHECK(order.toString().find("CLOSE_YESTERDAY") != std::string::npos);
}

void testMarketOrderBookContract() {
  Trading::MarketOrderBook snapshot_book(0);
  Exchange::MarketUpdate snapshot;
  snapshot.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  snapshot.depth_snapshot_.bids_[0] = {1233, 10};
  snapshot.depth_snapshot_.asks_[0] = {1235, 12};
  snapshot_book.onMarketUpdate(snapshot);

  CHECK(snapshot_book.getBBO()->bid_price_ == 1233);
  CHECK(snapshot_book.getBBO()->bid_qty_ == 10);
  CHECK(snapshot_book.getBBO()->ask_price_ == 1235);
  CHECK(snapshot_book.getBBO()->ask_qty_ == 12);

  Trading::MarketOrderBook incremental_book(0);
  Exchange::MarketUpdate add;
  add.type_ = Exchange::MarketUpdateType::ADD;
  add.order_id_ = 1;
  add.side_ = Common::Side::BUY;
  add.price_ = 1234;
  add.qty_ = 4;
  incremental_book.onMarketUpdate(add);
  CHECK(incremental_book.getBBO()->bid_price_ == 1234);
  CHECK(incremental_book.getBBO()->bid_qty_ == 4);

  add.type_ = Exchange::MarketUpdateType::CANCEL;
  incremental_book.onMarketUpdate(add);
  CHECK(incremental_book.getBBO()->bid_price_ == Common::Price_INVALID);
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
  CHECK(decision != nullptr);
  CHECK(decision->evaluation_id_ == 7);
  CHECK(decision->bias_ == Trading::JevBias::LONG);
  CHECK(decision->intent_ == Trading::JevIntent::OPEN);
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

  CHECK(engine.processPending());
  CHECK(order == "RMJ");
  CHECK(engine.marketOrderBook(0).getBBO()->bid_price_ == 100);
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
  CHECK(threw);
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
  CHECK(requests.size() == 1);
  CHECK(gateway.processPending());
  CHECK(responses.size() == 1);
  CHECK(engine.processPending());
  auto& open_order = engine.orderManager().getOrder(0, Common::Side::BUY);
  CHECK(open_order.order_state_ == Trading::OMOrderState::LIVE);

  venue.fill(open_order.order_id_, 100, 2);
  CHECK(engine.processPending());
  const auto& opened = engine.positionKeeper().getPositionInfo(0);
  CHECK(opened.position_ == 2);
  CHECK(opened.todayQty() == 2);

  engine.orderManager().moveCloseOrder(0, 101, Common::Side::SELL, 2);
  CHECK(requests.size() == 1);
  const auto& close_order = engine.orderManager().getOrder(0, Common::Side::SELL);
  CHECK(close_order.offset_ == Common::OrderOffset::CLOSE_TODAY);
  gateway.processPending();
  engine.processPending();
  venue.fill(close_order.order_id_, 101, 2);
  engine.processPending();
  const auto& closed = engine.positionKeeper().getPositionInfo(0);
  CHECK(closed.position_ == 0);
  CHECK(closed.todayQty() == 0);
  CHECK(closed.realized_pnl_ == 2.0);
}

void testQueueFacingAdapters() {
  Exchange::MarketUpdateLFQueue market_updates(8);
  Trading::MarketDataConsumer consumer(7, &market_updates);
  consumer.start();
  Exchange::MarketUpdate update;
  update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
  consumer.publishMarketUpdate(update);
  CHECK(consumer.localReceiveSequence() == 1);
  CHECK(market_updates.size() == 1);
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
  CHECK(gateway.processPending());
  CHECK(seen_sequence == 1);
  CHECK(gateway.nextOutgoingSequence() == 2);
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
  CHECK(worker.processPending());
  engine.registerEvaluation(0, 3);
  std::size_t accepted = 0;
  engine.onJevDecision = [&](const Trading::JevDecision& decision) {
    CHECK(decision.evaluation_id_ == 3);
    ++accepted;
  };
  CHECK(engine.processPending());
  CHECK(accepted == 1);

  auto* stale = decisions.getNextToWriteTo();
  stale->evaluation_id_ = 2;
  stale->ticker_id_ = 0;
  decisions.updateWriteIndex();
  CHECK(engine.processPending());
  CHECK(accepted == 1);
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
  CHECK(body.find("\"model\":\"jev-latest\"") != std::string::npos);
  CHECK(body.find("\"evaluation_id\": 9") != std::string::npos);
  CHECK(body.find("\"working_orders\"") != std::string::npos);
  CHECK(body.find("\"order_id\": 12") != std::string::npos);
  CHECK(body.find("\"open\":\"open a position\"") != std::string::npos);
  CHECK(body.find("\"hold\":\"take no action\"") != std::string::npos);
  CHECK(body.find("\"close\":\"reduce the current position\"") ==
         std::string::npos);

  state.position_.net_position_ = 2;
  const auto occupied_body = client.buildRequestBody(state);
  CHECK(occupied_body.find("\"close\":\"reduce the current position\"") !=
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
  CHECK(engine.processPending());
  CHECK(evaluations.size() == 1);

  CHECK(worker.processPending());
  CHECK(engine.processPending());
  CHECK(requests.size() == 1);
  const auto* request = requests.getNextToRead();
  CHECK(request != nullptr);
  CHECK(request->client_id_ == 7);
  CHECK(request->side_ == Common::Side::SELL);
  CHECK(request->price_ == 99);
  CHECK(request->offset_ == Common::OrderOffset::OPEN);
}

void testYesterdayFirstPositionClose() {
  Trading::PositionInfo position;
  position.position_ = 3;
  position.position_days_[static_cast<std::size_t>(Common::PositionDay::YESTERDAY)] =
      {2, 95.0};
  position.position_days_[static_cast<std::size_t>(Common::PositionDay::TODAY)] =
      {1, 100.0};

  CHECK(position.closeOffsetFor(1) == Common::OrderOffset::CLOSE_YESTERDAY);

  Exchange::ClientResponse response;
  response.type_ = Exchange::ClientResponseType::FILLED;
  response.ticker_id_ = 0;
  response.side_ = Common::Side::SELL;
  response.price_ = 97;
  response.exec_qty_ = 2;
  response.leaves_qty_ = 0;
  response.offset_ = Common::OrderOffset::CLOSE_YESTERDAY;
  position.onFill(response);

  CHECK(position.position_ == 1);
  CHECK(position.yesterdayQty() == 0);
  CHECK(position.todayQty() == 1);
  CHECK(position.realized_pnl_ == 4.0);
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
