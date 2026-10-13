#include "market_data_consumer.h"

#include <exception>
#include <stdexcept>

#include "common/time_utils.h"

namespace Trading {

// start() may run on a composition thread. Only publishing owns this handle.
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
    slot->published_ns_ = Common::nowSteadyNs();
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
