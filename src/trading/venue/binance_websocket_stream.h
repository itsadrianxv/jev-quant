#pragma once
#include <atomic>
#include <functional>
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
  private:
    auto run() -> void;
    std::string target_;
    MessageHandler on_message_;
    ErrorHandler on_error_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};
}



