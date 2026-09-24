#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <utility>

#include "common/lf_queue.h"
#include "common/types.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"

namespace Trading {

/// Queue-facing participant-side order boundary.
///
/// Transport-specific code installs a request handler and calls
/// publishClientResponse() after decoding a venue response. Sequence numbers
/// retain the OrderGateway contract from the reference implementation.
class OrderGateway final {
 public:
  using RequestHandler =
      std::function<void(std::size_t, const Exchange::ClientRequest&)>;

  OrderGateway(Common::ClientId client_id,
               Exchange::ClientRequestLFQueue* client_requests,
               Exchange::ClientResponseLFQueue* client_responses)
      : client_id_(client_id),
        outgoing_requests_(client_requests),
        incoming_responses_(client_responses) {}

  ~OrderGateway();

  OrderGateway(const OrderGateway&) = delete;
  OrderGateway& operator=(const OrderGateway&) = delete;
  OrderGateway(OrderGateway&&) = delete;
  OrderGateway& operator=(OrderGateway&&) = delete;

  auto start() -> void;
  auto stop() -> void;
  auto processPending() -> bool;

  auto publishClientResponse(std::size_t seq_num,
                            const Exchange::ClientResponse& response) -> void;

  void setRequestHandler(RequestHandler handler) {
    request_handler_ = std::move(handler);
  }

  [[nodiscard]] auto running() const noexcept -> bool {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto clientId() const noexcept -> Common::ClientId {
    return client_id_;
  }

  [[nodiscard]] auto nextOutgoingSequence() const noexcept -> std::size_t {
    return next_outgoing_seq_num_;
  }

  [[nodiscard]] auto nextExpectedResponseSequence() const noexcept
      -> std::size_t {
    return next_exp_seq_num_;
  }

 private:
  auto run() -> void;

  const Common::ClientId client_id_ = Common::ClientId_INVALID;
  Exchange::ClientRequestLFQueue* outgoing_requests_ = nullptr;
  Exchange::ClientResponseLFQueue* incoming_responses_ = nullptr;
  RequestHandler request_handler_;
  std::atomic<bool> running_{false};
  std::thread worker_;
  std::size_t next_outgoing_seq_num_ = 1;
  std::size_t next_exp_seq_num_ = 1;
};

}  // namespace Trading
