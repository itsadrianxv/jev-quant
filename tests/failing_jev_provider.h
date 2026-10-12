#pragma once

#include <stdexcept>

#include "trading/strategy/jev_worker.h"

namespace Tests {

/// Provider stub whose evaluate() always throws. Used to exercise JevWorker
/// failure accounting without network access.
class FailingJevProvider final : public Trading::JevDecisionProvider {
  public:
    auto evaluate(const Trading::JevEvaluationState&) -> Trading::JevDecision override {
        throw std::runtime_error("expected failure");
    }
};

}  // namespace Tests
