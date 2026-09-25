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
  while (running()) {
    if (!processPending()) {
      std::this_thread::yield();
    }
  }
}

auto JevWorker::processPending() -> bool {
  if (incoming_states_ == nullptr || outgoing_decisions_ == nullptr ||
      provider_ == nullptr) {
    throw std::logic_error("JevWorker dependencies must not be null");
  }

  const JevEvaluationState* newest = nullptr;
  JevEvaluationState latest;
  while (const auto* state = incoming_states_->getNextToRead()) {
    latest = *state;
    newest = &latest;
    incoming_states_->updateReadIndex();
  }
  if (newest == nullptr) return false;

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
  } catch (const std::exception&) {
    failure_count_.fetch_add(1, std::memory_order_acq_rel);
  }
  return true;
}

}  // namespace Trading
