#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

namespace Trading {
// Cross-worker control only. Books, orders, and input queues remain worker-owned.
struct SimNowControl {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::string error;
    std::atomic<bool> active{false}, fatal{false}, stopping{false}, overflow{false};
    std::atomic<bool> disconnected{false};
    bool stop_done = false;  // Protected by mutex.
    std::mutex market_state_mutex;
    struct MarketState {
        bool subscribed = false, quote_valid = false;
        std::string trading_day, market_day;
    } market_state;

    void signalFailure(const std::string& reason);
};
}  // namespace Trading
