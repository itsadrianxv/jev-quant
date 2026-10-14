#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include "trading/market_data/market_data_consumer.h"

namespace {
using namespace std::chrono_literals;

TEST(MarketDataWorkerTest, StartsOnceJoinsAndRejectsRestart) {
    Exchange::MarketUpdateLFQueue updates(8);
    Trading::MarketDataConsumer market(7, &updates);
    std::promise<std::thread::id> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    std::atomic<int> calls{0};
    std::atomic<bool> exited{false};
    market.setPollHandler([&] {
        if (calls.fetch_add(1) == 0) {
            entered.set_value(std::this_thread::get_id());
            EXPECT_EQ(std::future_status::ready, release_future.wait_for(5s));
        }
        exited.store(true);
    });
    market.stop(); // Stopping an unused component does not consume its lifecycle.
    market.start();
    const auto status = entered_future.wait_for(5s);
    EXPECT_EQ(std::future_status::ready, status);
    EXPECT_TRUE(market.running());
    market.start();
    EXPECT_EQ(1, calls.load());
    EXPECT_THROW(market.setPollHandler({}), std::logic_error);
    release.set_value();
    market.stop();
    ASSERT_EQ(std::future_status::ready, status);
    EXPECT_NE(std::this_thread::get_id(), entered_future.get());
    EXPECT_TRUE(exited.load());
    EXPECT_FALSE(market.running());
    const auto stopped_calls = calls.load();
    market.stop();
    EXPECT_EQ(stopped_calls, calls.load());
    EXPECT_THROW(market.start(), std::logic_error);
}

TEST(MarketDataWorkerTest, DestructionJoinsTheWorkerBeforeReleasingCallbackState) {
    Exchange::MarketUpdateLFQueue updates(8);
    std::atomic<unsigned> calls{0};
    {
        Trading::MarketDataConsumer market(7, &updates);
        market.setPollHandler([&] { ++calls; });
        market.start();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (calls.load() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        EXPECT_GT(calls.load(), 0U);
    }
    const auto stopped_calls = calls.load();
    std::this_thread::yield();
    EXPECT_EQ(stopped_calls, calls.load());
}
} // namespace
