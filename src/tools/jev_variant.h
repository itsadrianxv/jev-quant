#pragma once

#include <nlohmann/json.hpp>

namespace Trading::Tools {

/// Rewrites the wording fields that commit 381cd97 changed back to their
/// values from commit 262e223 (`git show 262e223:src/trading/strategy/jev_http_client.cpp`):
/// questions.bias.instructions, questions.intent.instructions, and, when the
/// position is not flat, questions.intent.criteria.close.  State, option
/// membership, and every other field are preserved verbatim.  A flat request
/// has no close description to change.
[[nodiscard]] auto buildPreviousWordingVariant(const nlohmann::json& request) -> nlohmann::json;

/// Exact leaf-level field diff between two request documents, as an array of
/// {"pointer", "from", "to"} entries.  Structural differences are reported as
/// {"pointer", "error"} entries instead of being silently normalized.
[[nodiscard]] auto describeWordingDiff(const nlohmann::json& before,
                                                                             const nlohmann::json& after) -> nlohmann::json;

}  // namespace Trading::Tools
