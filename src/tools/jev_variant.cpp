#include "jev_variant.h"

namespace Trading::Tools {
namespace {

// Wording pinned at the prior policy baseline 262e223.  Do not edit these
// strings by hand; they are byte-for-byte copies of the committed prior
// implementation cited in the header.
constexpr auto k262e223BiasInstructions = "long or short?";
constexpr auto k262e223IntentInstructions = "open, close, or hold?";
constexpr auto k262e223CloseCriteria = "reduce the current position";

auto diffJson(const nlohmann::json& before, const nlohmann::json& after,
              const std::string& pointer, nlohmann::json& out) -> void {
    if (before.is_object() && after.is_object()) {
        if (before.size() != after.size()) {
            out.push_back({{"pointer", pointer},
                                             {"error", "key set differs"},
                                             {"before_keys", before},
                                             {"after_keys", after}});
            return;
        }
        for (const auto& item : before.items()) {
            if (!after.contains(item.key())) {
                out.push_back({{"pointer", pointer + "/" + item.key()},
                                                 {"error", "key missing after"}});
                continue;
            }
            diffJson(item.value(), after.at(item.key()),
                             pointer.empty() ? "/" + item.key() : pointer + "/" + item.key(), out);
        }
        return;
    }
    if (before.is_array() && after.is_array()) {
        if (before.size() != after.size()) {
            out.push_back({{"pointer", pointer},
                                             {"error", "array length differs"},
                                             {"before", before},
                                             {"after", after}});
            return;
        }
        for (std::size_t index = 0; index < before.size(); ++index) {
            diffJson(before.at(index), after.at(index),
                             pointer + "/" + std::to_string(index), out);
        }
        return;
    }
    if (before != after) out.push_back({{"pointer", pointer}, {"from", before}, {"to", after}});
}

}  // namespace

auto buildPreviousWordingVariant(const nlohmann::json& request) -> nlohmann::json {
    auto variant = request;
    auto& bias = variant.at("questions").at("bias");
    auto& intent = variant.at("questions").at("intent");
    bias.at("instructions") = k262e223BiasInstructions;
    intent.at("instructions") = k262e223IntentInstructions;
    if (intent.at("criteria").contains("close")) {
        intent.at("criteria").at("close") = k262e223CloseCriteria;
    }
    return variant;
}

auto describeWordingDiff(const nlohmann::json& before, const nlohmann::json& after)
        -> nlohmann::json {
    nlohmann::json diff = nlohmann::json::array();
    diffJson(before, after, "", diff);
    return diff;
}

}  // namespace Trading::Tools
