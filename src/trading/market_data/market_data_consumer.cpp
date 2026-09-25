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
}

auto MarketDataConsumer::publishSequencedMarketUpdate(
        std::size_t seq_num, const Exchange::MarketUpdate& update) -> void {
    if (seq_num != next_expected_venue_seq_num_) {
        throw std::logic_error("MarketDataConsumer venue sequence gap");
    }
    ++next_expected_venue_seq_num_;
    publishMarketUpdate(update);
}

}  // namespace Trading
