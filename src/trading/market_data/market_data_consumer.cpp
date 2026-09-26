#include "market_data_consumer.h"

#include <exception>
#include <stdexcept>

namespace Trading {

auto MarketDataConsumer::publishMarketUpdate(
        const Exchange::MarketUpdate& update) -> void {
    if (market_updates_ == nullptr) {
        throw std::logic_error("MarketDataConsumer queue is null");
    }
    auto* slot = market_updates_->tryGetNextToWriteTo();
    if (slot == nullptr) {
        std::terminate();
    }
    *slot = update;
    market_updates_->updateWriteIndex();
    ++local_receive_seq_num_;
    if (log_handle_ && verbose_market_data_) {
        log_handle_->log(Common::LogLevel::DEBUG,
                         "event=market_update_published client_id=" +
                                 std::to_string(client_id_) + " ticker_id=" +
                                 std::to_string(update.ticker_id_));
    }
}

auto MarketDataConsumer::publishSequencedMarketUpdate(
        std::size_t seq_num, const Exchange::MarketUpdate& update) -> void {
    if (seq_num != next_expected_venue_seq_num_) {
        if (log_handle_) {
            log_handle_->log(Common::LogLevel::ERROR,
                             "event=market_sequence_gap client_id=" +
                                     std::to_string(client_id_) + " expected=" +
                                     std::to_string(next_expected_venue_seq_num_) +
                                     " actual=" + std::to_string(seq_num));
        }
        throw std::logic_error("MarketDataConsumer venue sequence gap");
    }
    ++next_expected_venue_seq_num_;
    publishMarketUpdate(update);
}

}  // namespace Trading
