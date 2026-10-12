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

TEST(JevDecisionParserTest, ResponseWithoutAnswersObjectIsRejected) {
    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision("{}", makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, MalformedPayloadIsRejected) {
    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision("{not json", makeState(0)),
                 Json::parse_error);
}

TEST(JevDecisionParserTest, MissingBiasAnswerIsRejected) {
    const auto body = responseAfter(
            [](Json& response) { response.at("answers").erase("bias"); });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, NonChoiceAnswerTypeIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias")["type"] = "text";
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, AnswerWithoutChoiceIsRejected) {
    const auto body = responseAfter(
            [](Json& response) { response.at("answers").at("bias").erase("choice"); });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, UnknownBiasChoiceIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias")["choice"] = "up";
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, UnknownIntentChoiceIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("intent")["choice"] = "wait";
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, AnswerWithoutProbabilitiesIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias").erase("probabilities");
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, AnswerWithoutConfidenceIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("intent").erase("confidence");
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, ConfidenceOutOfRangeIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("intent")["confidence"] = 1.5;
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, MissingProbabilityKeyIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias").at("probabilities").erase("short");
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, ProbabilityOutOfRangeIsRejected) {
    const auto body = responseAfter([](Json& response) {
        response.at("answers").at("bias").at("probabilities")["short"] = 1.5;
    });

    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(body, makeState(0)),
                 std::runtime_error);
}

TEST(JevDecisionParserTest, OccupiedPositionRequiresTheCloseProbability) {
    EXPECT_THROW((void)Trading::JevHttpClient::parseDecision(kFlatResponse,
                                                             makeState(2)),
                 std::runtime_error);
}

}  // namespace
