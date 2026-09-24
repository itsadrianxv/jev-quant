#pragma once

#include <atomic>
#include <cstddef>

#include "common/lf_queue.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"

namespace Trading {

/// Queue-facing participant-side market-data boundary.
///
/// Transport-specific code will call publishMarketUpdate() after decoding a
/// venue message. The first implementation deliberately leaves sockets out so
/// replay and simulated venues can exercise the same queue contract.
class MarketDataConsumer final {
 public:
  MarketDataConsumer(Common::ClientId client_id,
                     Exchange::MarketUpdateLFQueue* market_updates)
      : client_id_(client_id), market_updates_(market_updates) {}

  MarketDataConsumer(const MarketDataConsumer&) = delete;
  MarketDataConsumer& operator=(const MarketDataConsumer&) = delete;
  MarketDataConsumer(MarketDataConsumer&&) = delete;
  MarketDataConsumer& operator=(MarketDataConsumer&&) = delete;

  auto start() noexcept -> void { running_.store(true, std::memory_order_release); }
  auto stop() noexcept -> void { running_.store(false, std::memory_order_release); }

  auto publishMarketUpdate(const Exchange::MarketUpdate& update) -> void;

  /// Accept a venue sequence when the transport supplies one.
  auto publishSequencedMarketUpdate(std::size_t seq_num,
                                    const Exchange::MarketUpdate& update)
      -> void;

  [[nodiscard]] auto running() const noexcept -> bool {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto clientId() const noexcept -> Common::ClientId {
    return client_id_;
  }

  [[nodiscard]] auto localReceiveSequence() const noexcept -> std::size_t {
    return local_receive_seq_num_;
  }

  [[nodiscard]] auto nextExpectedVenueSequence() const noexcept -> std::size_t {
    return next_expected_venue_seq_num_;
  }

 private:
  const Common::ClientId client_id_ = Common::ClientId_INVALID;
  Exchange::MarketUpdateLFQueue* market_updates_ = nullptr;
  std::atomic<bool> running_{false};
  std::size_t local_receive_seq_num_ = 0;
  std::size_t next_expected_venue_seq_num_ = 1;
};

}  // namespace Trading
