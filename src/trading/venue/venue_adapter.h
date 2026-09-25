#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <iostream>

#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"

namespace Trading {

class VenueAdapter {
 public:
  virtual ~VenueAdapter() = default;
  virtual auto start() -> void = 0;
  virtual auto stop() -> void = 0;
  [[nodiscard]] virtual auto running() const noexcept -> bool = 0;
};

/// Production-shaped seam for venue connectivity.
///
/// TODO: replace this adapter with the venue transport and sequencing
/// implementation. The placeholder deliberately consumes client requests but
/// does not manufacture market data or client responses.
class UnimplementedVenueAdapter final : public VenueAdapter {
 public:
  UnimplementedVenueAdapter(MarketDataConsumer* market_data,
                            OrderGateway* order_gateway)
      : market_data_(market_data), order_gateway_(order_gateway) {}

  UnimplementedVenueAdapter(const UnimplementedVenueAdapter&) = delete;
  UnimplementedVenueAdapter& operator=(const UnimplementedVenueAdapter&) = delete;

  auto start() -> void override {
    if (order_gateway_ == nullptr || market_data_ == nullptr) return;
    order_gateway_->setRequestHandler(
        [this](std::size_t, const Exchange::ClientRequest&) {
          // TODO: send the request through the venue order transport.
          ++unconnected_request_count_;
          std::clog << "Venue adapter is unimplemented; client request was not sent\n";
        });
    order_gateway_->start();
    market_data_->start();
    running_.store(true, std::memory_order_release);
  }

  auto stop() -> void override {
    running_.store(false, std::memory_order_release);
    if (market_data_ != nullptr) market_data_->stop();
    if (order_gateway_ != nullptr) order_gateway_->stop();
  }

  [[nodiscard]] auto running() const noexcept -> bool override {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto unconnectedRequestCount() const noexcept -> std::size_t {
    return unconnected_request_count_.load(std::memory_order_acquire);
  }

 private:
  MarketDataConsumer* market_data_ = nullptr;
  OrderGateway* order_gateway_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<std::size_t> unconnected_request_count_{0};
};

}  // namespace Trading
