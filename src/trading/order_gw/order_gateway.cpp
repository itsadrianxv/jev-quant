#include "order_gateway.h"

#include <exception>
#include <stdexcept>

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
    worker_ = std::thread([this] { run(); });
}

auto OrderGateway::stop() -> void {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

auto OrderGateway::run() -> void {
    if (logger_ != nullptr) {
        log_handle_.emplace(logger_->registerProducer(
                "OrderGateway", "order-gateway-" + std::to_string(client_id_) + ".log"));
        log_handle_->bindToCurrentThread();
        log_handle_->log(Common::LogLevel::INFO,
                         "event=component_started client_id=" + std::to_string(client_id_));
    }
    while (running()) {
        if (poll_handler_) poll_handler_();
        if (!processPending()) {
            std::this_thread::yield();
        }
    }
    if (log_handle_) {
        log_handle_->log(Common::LogLevel::INFO,
                         "event=component_stopped client_id=" + std::to_string(client_id_));
    }
}

auto OrderGateway::processPending() -> bool {
    if (outgoing_requests_ == nullptr) {
        throw std::logic_error("OrderGateway request queue is null");
    }

    bool processed = false;
    while (const auto* request = outgoing_requests_->getNextToRead()) {
        const auto sequence = next_outgoing_seq_num_++;
        if (log_handle_) {
            log_handle_->log(Common::LogLevel::INFO,
                             "event=client_request_consumed client_id=" +
                                     std::to_string(client_id_) + " sequence=" +
                                     std::to_string(sequence));
        }
        if (request_handler_) {
            request_handler_(sequence, *request);
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
            log_handle_->log(Common::LogLevel::ERROR,
                             "event=response_sequence_gap client_id=" +
                                     std::to_string(client_id_) + " expected=" +
                                     std::to_string(next_exp_seq_num_) + " actual=" +
                                     std::to_string(seq_num));
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
        log_handle_->log(Common::LogLevel::INFO,
                         "event=client_response_published client_id=" +
                                 std::to_string(client_id_) + " sequence=" +
                                 std::to_string(seq_num));
    }
}

}  // namespace Trading
