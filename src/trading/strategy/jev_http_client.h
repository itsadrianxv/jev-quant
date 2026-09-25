#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "jev_worker.h"

namespace Trading {

struct JevHttpConfig {
  std::string endpoint_ = "https://api.typesafe.ai/v1/systemone";
  std::string api_key_;
  std::string model_ = "jev-latest";
  long timeout_ms_ = 4000;
};

/// Direct HTTP DecisionProvider for the documented TypeSafe System One API.
class JevHttpClient final : public JevDecisionProvider {
 public:
  explicit JevHttpClient(JevHttpConfig config) : config_(std::move(config)) {}

  auto evaluate(const JevEvaluationState& state) -> JevDecision override;

  [[nodiscard]] auto buildRequestBody(const JevEvaluationState& state) const
      -> std::string;

 private:
  JevHttpConfig config_;

  static auto parseDecision(const std::string& body,
                            const JevEvaluationState& state) -> JevDecision;
};

}  // namespace Trading
