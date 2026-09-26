#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <nlohmann/json.hpp>
namespace Trading {
class BinanceWebSocketStream final {
  public:
    using MessageHandler = std::function<void(const nlohmann::json&)>;
    using ErrorHandler = std::function<void(const std::string&)>;
    BinanceWebSocketStream(std::string target, MessageHandler on_message, ErrorHandler on_error);
    ~BinanceWebSocketStream();
    BinanceWebSocketStream(const BinanceWebSocketStream&) = delete;
    auto start() -> void;
    auto stop() noexcept -> void;
    auto waitUntilConnected(std::chrono::milliseconds timeout) -> bool;
  private:
    auto run() -> void;
    std::string target_;
    MessageHandler on_message_;
    ErrorHandler on_error_;
    std::atomic<bool> stopping_{false};
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    bool connected_ = false;
    std::atomic<int> socket_fd_{-1};
    std::thread worker_;
};
}



