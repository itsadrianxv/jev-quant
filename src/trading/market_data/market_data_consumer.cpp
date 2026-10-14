#include "market_data_consumer.h"

#include <exception>
#include <stdexcept>

#include "common/time_utils.h"

namespace Trading {

MarketDataConsumer::~MarketDataConsumer() { stop(); }

auto MarketDataConsumer::start() -> void {
    if (running()) return;
    if (started_) throw std::logic_error("MarketDataConsumer cannot be restarted");
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::jthread([this] { run(); });
        started_ = true;
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }
}

auto MarketDataConsumer::stop() noexcept -> void {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

auto MarketDataConsumer::setPollHandler(std::function<void()> handler) -> void {
    if (running()) throw std::logic_error("Cannot replace a running market poll handler");
    poll_handler_ = std::move(handler);
}

auto MarketDataConsumer::run() -> void {
    ensureLoggingStarted();
    while (running()) {
        if (poll_handler_) poll_handler_();
        std::this_thread::yield();
    }
    if (log_handle_) {
        log_handle_->logFixed(Common::LogLevel::INFO, [this](auto& out) {
            out.append("event=component_stopped client_id=").append(client_id_);
        });
    }
}

// start() runs on the lifecycle owner; logging binds on the worker or synchronous publisher.
auto MarketDataConsumer::ensureLoggingStarted() -> void {
    if (log_handle_ && !logging_started_) {
        log_handle_->bindToCurrentThread();
        log_handle_->log(Common::LogLevel::INFO,
                         "event=component_started client_id=" + std::to_string(client_id_));
        logging_started_ = true;
    }
}

auto MarketDataConsumer::publishMarketUpdate(
        const Exchange::MarketUpdate& update) -> void {
    if (market_updates_ == nullptr) {
        throw std::logic_error("MarketDataConsumer queue is null");
    }
    ensureLoggingStarted();
    auto* slot = market_updates_->tryGetNextToWriteTo();
    if (slot == nullptr) {
        std::terminate();
    }
    *slot = update;
    if (slot->trace_.source_id) slot->trace_.t[2] = Common::nowSteadyNs();
    market_updates_->updateWriteIndex();
    ++local_receive_seq_num_;
    if (log_handle_ && verbose_market_data_) {
        log_handle_->logFixed(Common::LogLevel::DEBUG,
                              [&](auto& out) {
                                  out.append("event=market_update_published client_id=")
                                          .append(client_id_)
                                          .append(" ticker_id=")
                                          .append(update.ticker_id_);
                              });
    }
}

auto MarketDataConsumer::publishSequencedMarketUpdate(
        std::size_t seq_num, const Exchange::MarketUpdate& update) -> void {
    ensureLoggingStarted();
    if (seq_num != next_expected_venue_seq_num_) {
        if (log_handle_) {
            log_handle_->logFixed(Common::LogLevel::ERROR,
                                  [&](auto& out) {
                                      out.append("event=market_sequence_gap client_id=")
                                              .append(client_id_)
                                              .append(" expected=")
                                              .append(next_expected_venue_seq_num_)
                                              .append(" actual=")
                                              .append(seq_num);
                                  });
        }
        throw std::logic_error("MarketDataConsumer venue sequence gap");
    }
    ++next_expected_venue_seq_num_;
    publishMarketUpdate(update);
}

}  // namespace Trading
