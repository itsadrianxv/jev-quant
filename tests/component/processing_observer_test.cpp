#include <array>
#include "harness.h"
#include "common/time_utils.h"

namespace {
class Observer final : public Trading::ProcessingObserver {
  public:
    std::array<Trading::ProcessingSample, 16> samples{};
    std::size_t count = 0;
    void record(const Trading::ProcessingSample& sample) noexcept override {
        if (count < samples.size()) samples[count++] = sample;
    }
};

TEST(ProcessingObserverTest, IncludesBusinessCallbackAndExcludesObserverStorage) {
    Observer observer;
    auto h = Tests::makeHarness(Tests::EngineHarness::defaultLimits());
    h->engine.setProcessingObserver(&observer);
    std::uint64_t callback_started = 0, callback_finished = 0;
    h->engine.onMarketUpdate = [&](const auto&, const auto& book) {
        callback_started = Common::nowSteadyNs();
        EXPECT_EQ(book.getBBO()->bid_price_, 100);
        EXPECT_EQ(observer.count, 0U);
        callback_finished = Common::nowSteadyNs();
    };
    Tests::writeMarketUpdate(h->market_updates, 0, 100, 101);
    h->engine.processPending();
    ASSERT_EQ(observer.count, 1U);
    EXPECT_EQ(observer.samples[0].stage, Trading::ProcessingStage::MARKET);
    EXPECT_GE(observer.samples[0].elapsed_ns, callback_finished - callback_started);
    h->engine.setProcessingObserver(nullptr);
    h->engine.onMarketUpdate = [](const auto&, const auto&) {};
    Tests::writeMarketUpdate(h->market_updates, 0, 102, 103);
    h->engine.processPending();
    EXPECT_EQ(observer.count, 1U);
    EXPECT_EQ(h->engine.marketOrderBook(0).getBBO()->bid_price_, 102);
}

TEST(ProcessingObserverTest, RecordsEarlyExitsAndActualRequestEmissionSeparately) {
    Observer observer;
    auto h = Tests::makeHarness(Tests::EngineHarness::defaultLimits());
    Tests::writeMarketUpdate(h->market_updates, 0, 100, 101);
    h->engine.processPending();
    h->engine.setProcessingObserver(&observer);
    // An unregistered evaluation is rejected before the decision callback.
    Tests::writeDecision(h->decisions, 99);
    h->engine.processPending();
    h->engine.registerEvaluation(0, 1);
    Tests::writeDecision(h->decisions, 1, Trading::JevBias::LONG, Trading::JevIntent::HOLD);
    h->engine.processPending();
    Tests::writeDecision(h->decisions, 1);
    h->engine.processPending();
    // A second OPEN while PENDING_NEW must not pretend to send a new order.
    Tests::writeDecision(h->decisions, 1);
    h->engine.processPending();
    ASSERT_EQ(observer.count, 4U);
    EXPECT_EQ(observer.samples[0].request_count, 0U);
    EXPECT_EQ(observer.samples[1].kind, static_cast<unsigned>(Trading::JevIntent::HOLD));
    EXPECT_EQ(observer.samples[1].request_count, 0U);
    EXPECT_EQ(observer.samples[2].request_count, 1U);
    EXPECT_EQ(observer.samples[2].request_type, static_cast<unsigned>(Exchange::ClientRequestType::NEW));
    EXPECT_EQ(observer.samples[3].request_count, 0U);
    EXPECT_EQ(h->requests.size(), 1U);
}

TEST(ProcessingObserverTest, RecordsEachResponseAfterOrderAndPositionUpdates) {
    Observer observer;
    auto h = Tests::makeHarness(Tests::EngineHarness::defaultLimits());
    Tests::writeMarketUpdate(h->market_updates, 0, 100, 101);
    h->engine.processPending();
    h->engine.registerEvaluation(0, 1);
    Tests::writeDecision(h->decisions, 1);
    h->engine.processPending();
    const auto request = Tests::readRequest(h->requests);
    h->engine.setProcessingObserver(&observer);
    unsigned callbacks = 0;
    h->engine.onClientResponse = [&](const auto& response) {
        EXPECT_EQ(observer.count, callbacks++);
        if (response.type_ == Exchange::ClientResponseType::FILLED) {
            EXPECT_EQ(h->engine.positionKeeper().getPositionInfo(0).position_, 1);
        }
    };
    Tests::writeResponse(h->responses, Exchange::ClientResponseType::ACCEPTED,
                         request.order_id_, request.side_, request.price_, 0, 1);
    Tests::writeResponse(h->responses, Exchange::ClientResponseType::FILLED,
                         request.order_id_, request.side_, request.price_, 1, 0);
    h->engine.processPending();
    ASSERT_EQ(observer.count, 2U);
    EXPECT_EQ(observer.samples[0].kind, static_cast<unsigned>(Exchange::ClientResponseType::ACCEPTED));
    EXPECT_EQ(observer.samples[1].kind, static_cast<unsigned>(Exchange::ClientResponseType::FILLED));
    EXPECT_EQ(h->responses.size(), 0U);
}
}  // namespace
