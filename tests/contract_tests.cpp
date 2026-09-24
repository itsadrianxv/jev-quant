#include <cassert>
#include <atomic>
#include <string>
#include <thread>

#include "common/lf_queue.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/strategy/jev_decision.h"
#include "trading/strategy/market_order_book.h"
#include "trading/strategy/om_order.h"
#include "trading/strategy/trade_engine.h"

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
  Trading::TradeEngine engine(&requests, &responses, &market_updates, &decisions);

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

}  // namespace

int main() {
  testQueueContract();
  testQueueSpscConcurrency();
  testMessageContract();
  testMarketOrderBookContract();
  testJevDecisionContract();
  testTradeEngineDispatchOrder();
  return 0;
}
