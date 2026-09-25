#pragma once

#include <atomic>
#include <thread>

#include "jev_decision.h"

namespace Trading {

class JevDecisionProvider {
 public:
  virtual ~JevDecisionProvider() = default;
  virtual auto evaluate(const JevEvaluationState& state) -> JevDecision = 0;
};

/// Deterministic provider used by replay and contract tests.
class MockJevDecisionProvider final : public JevDecisionProvider {
 public:
  auto evaluate(const JevEvaluationState& state) -> JevDecision override {
    JevDecision decision;
    decision.evaluation_id_ = state.evaluation_id_;
    decision.ticker_id_ = state.ticker_id_;
    decision.bias_ = state.position_.net_position_ < 0 ? JevBias::LONG
                                                       : JevBias::SHORT;
    decision.intent_ = state.position_.net_position_ == 0 ? JevIntent::OPEN
                                                           : JevIntent::HOLD;
    decision.long_probability_ = decision.bias_ == JevBias::LONG ? 0.75 : 0.25;
    decision.short_probability_ = 1.0 - decision.long_probability_;
    decision.open_probability_ = decision.intent_ == JevIntent::OPEN ? 0.75 : 0.1;
    decision.close_probability_ = 0.1;
    decision.hold_probability_ = decision.intent_ == JevIntent::HOLD ? 0.8 : 0.15;
    decision.confidence_ = 0.75;
    return decision;
  }
};

class JevWorker final {
 public:
  JevWorker(JevEvaluationStateLFQueue* incoming_states,
            JevDecisionLFQueue* outgoing_decisions,
            JevDecisionProvider* provider)
      : incoming_states_(incoming_states),
        outgoing_decisions_(outgoing_decisions),
        provider_(provider) {}

  ~JevWorker();

  JevWorker(const JevWorker&) = delete;
  JevWorker& operator=(const JevWorker&) = delete;
  JevWorker(JevWorker&&) = delete;
  JevWorker& operator=(JevWorker&&) = delete;

  auto start() -> void;
  auto stop() -> void;
  auto processPending() -> bool;

  [[nodiscard]] auto running() const noexcept -> bool {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto failureCount() const noexcept -> std::uint64_t {
    return failure_count_.load(std::memory_order_acquire);
  }

 private:
  auto run() -> void;

  JevEvaluationStateLFQueue* incoming_states_ = nullptr;
  JevDecisionLFQueue* outgoing_decisions_ = nullptr;
  JevDecisionProvider* provider_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> failure_count_{0};
  std::thread worker_;
};

}  // namespace Trading
