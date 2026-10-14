#include "order_gateway.h"

#include <exception>
#include <stdexcept>

#include "common/time_utils.h"

namespace Trading {

OrderGateway::~OrderGateway() {
    stop();
}

auto OrderGateway::start() -> void {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                                                                std::memory_order_acq_rel)) {
        return;
    }
    try {
        worker_ = std::jthread([this] { run(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }
}

auto OrderGateway::stop() -> void {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

auto OrderGateway::run() -> void {
    if (log_handle_) {
        log_handle_->bindToCurrentThread();
        log_handle_->logFixed(Common::LogLevel::INFO, [this](auto& out) {
            out.append("event=component_started client_id=").append(client_id_);
        });
    }
    while (running()) {
        if (poll_handler_) poll_handler_();
        if (!processPending()) {
            std::this_thread::yield();
        }
    }
    if (log_handle_) {
        log_handle_->logFixed(Common::LogLevel::INFO, [this](auto& out) {
            out.append("event=component_stopped client_id=").append(client_id_);
        });
    }
}

auto OrderGateway::processPending() -> bool {
    if (outgoing_requests_ == nullptr) {
        throw std::logic_error("OrderGateway request queue is null");
    }

    bool processed = false;
    while (const auto* request = outgoing_requests_->getNextToRead()) {
        const auto received_ns = request->trace_.t[4] ? Common::nowSteadyNs() : 0;
        auto observed = *request;
        if (observed.trace_.t[4]) observed.trace_.t[5] = received_ns;
        const auto sequence = next_outgoing_seq_num_++;
        if (log_handle_) {
            log_handle_->logFixed(Common::LogLevel::INFO,
                                  [&](auto& out) {
                                      out.append("event=client_request_consumed client_id=")
                                              .append(client_id_)
                                              .append(" sequence=")
                                              .append(sequence);
                                  });
        }
        if (request_handler_) {
            if (observed.trace_.t[4]) observed.trace_.t[6] = Common::nowSteadyNs();
            request_handler_(sequence, observed);
        }
        outgoing_requests_->updateReadIndex();
        processed = true;
    }
    return processed;
}

auto OrderGateway::publishClientResponse(
        std::size_t seq_num, const Exchange::ClientResponse& response) -> void {
    if (incoming_responses_ == nullptr) {
        throw std::logic_error("OrderGateway response queue is null");
    }
    if (response.client_id_ != client_id_) {
        throw std::logic_error("OrderGateway response client id mismatch");
    }
    if (seq_num != next_exp_seq_num_) {
        if (log_handle_) {
            log_handle_->logFixed(Common::LogLevel::ERROR,
                                  [&](auto& out) {
                                      out.append("event=response_sequence_gap client_id=")
                                              .append(client_id_)
                                              .append(" expected=")
                                              .append(next_exp_seq_num_)
                                              .append(" actual=")
                                              .append(seq_num);
                                  });
        }
        throw std::logic_error("OrderGateway response sequence gap");
    }

    auto* slot = incoming_responses_->tryGetNextToWriteTo();
    if (slot == nullptr) {
        std::terminate();
    }
    *slot = response;
    incoming_responses_->updateWriteIndex();
    ++next_exp_seq_num_;
    if (log_handle_) {
        log_handle_->logFixed(Common::LogLevel::INFO,
                              [&](auto& out) {
                                  out.append("event=client_response_published client_id=")
                                          .append(client_id_)
                                          .append(" sequence=")
                                          .append(seq_num);
                              });
    }
}

}  // namespace Trading
