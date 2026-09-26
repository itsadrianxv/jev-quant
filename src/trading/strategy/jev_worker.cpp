#include "jev_worker.h"

#include <exception>
#include <stdexcept>

namespace Trading {

JevWorker::~JevWorker() {
    stop();
}

auto JevWorker::start() -> void {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                                                                std::memory_order_acq_rel)) {
        return;
    }
    worker_ = std::thread([this] { run(); });
}

auto JevWorker::stop() -> void {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

auto JevWorker::run() -> void {
    if (logger_ != nullptr) {
        log_handle_.emplace(logger_->registerProducer("JevWorker", "jev-worker.log"));
        log_handle_->bindToCurrentThread();
        log_handle_->log(Common::LogLevel::INFO, "event=component_started");
    }
    while (running()) {
        if (!processPending()) {
            std::this_thread::yield();
        }
    }
    if (log_handle_) {
        log_handle_->log(Common::LogLevel::INFO, "event=component_stopped");
    }
}

auto JevWorker::processPending() -> bool {
    if (incoming_states_ == nullptr || outgoing_decisions_ == nullptr ||
            provider_ == nullptr) {
        throw std::logic_error("JevWorker dependencies must not be null");
    }

    const JevEvaluationState* newest = nullptr;
    JevEvaluationState latest;
    std::size_t drained = 0;
    while (const auto* state = incoming_states_->getNextToRead()) {
        latest = *state;
        newest = &latest;
        ++drained;
        incoming_states_->updateReadIndex();
    }
    if (newest == nullptr) return false;
    if (evaluation_filter_ && !evaluation_filter_(*newest)) return true;
    if (log_handle_) {
        log_handle_->log(Common::LogLevel::INFO,
                         "event=evaluation_batch_drained count=" +
                                 std::to_string(drained) + " latest_evaluation_id=" +
                                 std::to_string(newest->evaluation_id_));
    }

    try {
        auto decision = provider_->evaluate(*newest);
        if (decision.evaluation_id_ == 0) {
            decision.evaluation_id_ = newest->evaluation_id_;
        }
        if (decision.ticker_id_ == Common::TickerId_INVALID) {
            decision.ticker_id_ = newest->ticker_id_;
        }

        auto* slot = outgoing_decisions_->tryGetNextToWriteTo();
        if (slot == nullptr) {
            std::terminate();
        }
        *slot = decision;
        outgoing_decisions_->updateWriteIndex();
        if (log_handle_) {
            log_handle_->log(Common::LogLevel::INFO,
                             "event=decision_enqueued evaluation_id=" +
                                     std::to_string(decision.evaluation_id_));
        }
    } catch (const std::exception&) {
        failure_count_.fetch_add(1, std::memory_order_acq_rel);
        if (log_handle_) {
            log_handle_->log(Common::LogLevel::ERROR, "event=provider_failure");
        }
    }
    return true;
}

}  // namespace Trading
