#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "jev_worker.h"

namespace Trading {

struct JevHttpConfig {
    std::string endpoint_ = "https://openrouter.ai/api/alpha/decisions";
    std::string api_key_;
    std::string model_ = "typesafe/jev-1.13";
    long timeout_ms_ = 4000;
    unsigned max_retries_ = 0;
    std::string http_referer_;
    std::string http_title_;
};

/// C++ HTTP DecisionProvider for Jev through OpenRouter's Decisions API.
class JevHttpClient final : public JevDecisionProvider {
  public:
    explicit JevHttpClient(JevHttpConfig config) : config_(std::move(config)) {}

    auto evaluate(const JevEvaluationState& state) -> JevDecision override;

    [[nodiscard]] auto buildRequestBody(const JevEvaluationState& state) const
            -> std::string;

    static auto parseDecision(const std::string& body,
                                                        const JevEvaluationState& state) -> JevDecision;

  private:
    JevHttpConfig config_;
};

}  // namespace Trading
