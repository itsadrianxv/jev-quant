#include <gtest/gtest.h>

#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "trading/strategy/jev_http_client.h"

namespace {

using Json = nlohmann::json;

Trading::JevEvaluationState makeState(std::int32_t net_position) {
    Trading::JevEvaluationState state;
    state.evaluation_id_ = 9;
    state.ticker_id_ = 0;
    state.position_.net_position_ = net_position;
    return state;
}

const char* kFlatResponse = R"({
    "answers": {
        "bias": {
            "type": "choice", "choice": "long",
            "probabilities": {"long": 0.9, "short": 0.1}, "confidence": 0.8
        },
        "intent": {
            "type": "choice", "choice": "hold",
            "probabilities": {"open": 0.2, "hold": 0.8}, "confidence": 0.7
        }
    }
})";

const char* kOccupiedResponse = R"({
    "answers": {
        "bias": {
            "type": "choice", "choice": "short",
            "probabilities": {"long": 0.2, "short": 0.8}, "confidence": 0.6
        },
        "intent": {
            "type": "choice", "choice": "close",
            "probabilities": {"open": 0.1, "close": 0.7, "hold": 0.2},
            "confidence": 0.55
        }
    }
})";

auto responseAfter(const std::function<void(Json&)>& mutation) -> std::string {
    Json response = Json::parse(kFlatResponse);
    mutation(response);
    return response.dump();
}

// A well-formed flat-position response parses into a LONG/HOLD decision with
// all probabilities, confidence, and the request's evaluation/ticker ids
// carried through.
TEST(JevDecisionParserTest, FlatResponseParsesToAHoldDecision) {
    const auto decision = Trading::JevHttpClient::parseDecision(kFlatResponse,
                                                                makeState(0));

    EXPECT_EQ(9u, decision.evaluation_id_);
    EXPECT_EQ(0u, decision.ticker_id_);
    EXPECT_EQ(Trading::JevBias::LONG, decision.bias_);
    EXPECT_EQ(Trading::JevIntent::HOLD, decision.intent_);
    EXPECT_DOUBLE_EQ(0.9, decision.long_probability_);
    EXPECT_DOUBLE_EQ(0.1, decision.short_probability_);
    EXPECT_DOUBLE_EQ(0.2, decision.open_probability_);
    EXPECT_DOUBLE_EQ(0.0, decision.close_probability_);
    EXPECT_DOUBLE_EQ(0.8, decision.hold_probability_);
    EXPECT_DOUBLE_EQ(0.7, decision.confidence_);
}

// A well-formed occupied-position response parses into a SHORT/CLOSE
// decision, proving the close probability and per-answer confidence are
// extracted correctly.
TEST(JevDecisionParserTest, OccupiedResponseParsesToACloseDecision) {
    const auto decision = Trading::JevHttpClient::parseDecision(
            kOccupiedResponse, makeState(2));

    EXPECT_EQ(9u, decision.evaluation_id_);
    EXPECT_EQ(0u, decision.ticker_id_);
    EXPECT_EQ(Trading::JevBias::SHORT, decision.bias_);
    EXPECT_EQ(Trading::JevIntent::CLOSE, decision.intent_);
    EXPECT_DOUBLE_EQ(0.2, decision.long_probability_);
    EXPECT_DOUBLE_EQ(0.8, decision.short_probability_);
    EXPECT_DOUBLE_EQ(0.1, decision.open_probability_);
    EXPECT_DOUBLE_EQ(0.7, decision.close_probability_);
    EXPECT_DOUBLE_EQ(0.2, decision.hold_probability_);
    EXPECT_DOUBLE_EQ(0.55, decision.confidence_);
}

// A response missing the entire "answers" object is a contract violation
// and must be rejected with a runtime error.
TEST(JevDecisionParserTest, ResponseWithoutAnswersObjectIsRejected) {
    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision("{}", makeState(0)),
                 std::runtime_error);
}

// A payload that is not valid JSON at all surfaces the JSON parse error.
TEST(JevDecisionParserTest, MalformedPayloadIsRejected) {
    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision("{not json", makeState(0)),
                 Json::parse_error);
}

// A response with only the intent answer (no bias) cannot produce a
// decision and must be rejected.
TEST(JevDecisionParserTest, MissingBiasAnswerIsRejected) {
    const auto body = responseAfter(
            [](Json& response) { response.at("answers").erase("bias"); });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// An answer whose type is not "choice" (e.g. "text") is outside the
// expected schema and must be rejected.
TEST(JevDecisionParserTest, NonChoiceAnswerTypeIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias")["type"] = "text";
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// A choice-type answer without the actual "choice" field is incomplete and
// must be rejected.
TEST(JevDecisionParserTest, AnswerWithoutChoiceIsRejected) {
    const auto body = responseAfter(
            [](Json& response) { response.at("answers").at("bias").erase("choice"); });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// A bias choice outside {long, short} (e.g. "up") is invalid and must be
// rejected.
TEST(JevDecisionParserTest, UnknownBiasChoiceIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias")["choice"] = "up";
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// An intent choice outside {open, close, hold} (e.g. "wait") is invalid and
// must be rejected.
TEST(JevDecisionParserTest, UnknownIntentChoiceIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("intent")["choice"] = "wait";
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// An answer without the "probabilities" object is incomplete and must be
// rejected.
TEST(JevDecisionParserTest, AnswerWithoutProbabilitiesIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias").erase("probabilities");
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// An answer without the "confidence" field is incomplete and must be
// rejected.
TEST(JevDecisionParserTest, AnswerWithoutConfidenceIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("intent").erase("confidence");
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// A confidence outside [0, 1] (e.g. 1.5) is not a valid probability and
// must be rejected.
TEST(JevDecisionParserTest, ConfidenceOutOfRangeIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("intent")["confidence"] = 1.5;
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// A probabilities object missing one of the expected keys (e.g. "short")
// must be rejected instead of silently defaulting.
TEST(JevDecisionParserTest, MissingProbabilityKeyIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias").at("probabilities").erase("short");
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// A probability outside [0, 1] (e.g. 1.5) must be rejected.
TEST(JevDecisionParserTest, ProbabilityOutOfRangeIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias").at("probabilities")["short"] = 1.5;
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

// For an occupied position the flat response lacks the "close" probability
// key, which the parser requires in that state, so it must be rejected.
TEST(JevDecisionParserTest, OccupiedPositionRequiresTheCloseProbability) {
    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(kFlatResponse,
                                                             makeState(2)),
                 std::runtime_error);
}

}  // namespace
