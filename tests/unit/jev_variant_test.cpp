#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "trading/strategy/jev_http_client.h"
#include "tools/jev_variant.h"

namespace {

using Json = nlohmann::json;

auto buildRequest(std::int32_t net_position) -> Json {
    Trading::JevEvaluationState state;
    state.evaluation_id_ = 9;
    state.ticker_id_ = 0;
    state.position_.net_position_ = net_position;
    state.depth_snapshot_.last_price_ = 3500;
    state.depth_snapshot_.bids_[0] = {3499, 2};
    state.depth_snapshot_.asks_[0] = {3501, 2};

    const Trading::JevHttpClient client(Trading::JevHttpConfig{});
    return Json::parse(client.buildRequestBody(state));
}

// For a flat position the wording variant rewrites only the two instruction
// strings (bias and intent) to the simpler "previous" wording; state and the
// intent criteria are untouched.
TEST(JevWordingVariantTest, FlatVariantRewritesOnlyTheInstructionWording) {
    const auto request = buildRequest(0);

    const auto variant = Trading::Tools::buildPreviousWordingVariant(request);

    EXPECT_EQ(request.at("state"), variant.at("state"));
    EXPECT_EQ("long or short?",
              variant.at("questions").at("bias").at("instructions").get<std::string>());
    EXPECT_EQ("open, close, or hold?",
              variant.at("questions").at("intent").at("instructions").get<std::string>());
    EXPECT_EQ(request.at("questions").at("intent").at("criteria"),
              variant.at("questions").at("intent").at("criteria"));
}

// For an occupied position the variant additionally rewrites the intent
// "close" criteria to "reduce the current position"; state stays untouched.
TEST(JevWordingVariantTest, OccupiedVariantRewritesTheCloseCriteriaToo) {
    const auto request = buildRequest(2);

    const auto variant = Trading::Tools::buildPreviousWordingVariant(request);

    EXPECT_EQ(request.at("state"), variant.at("state"));
    EXPECT_EQ("reduce the current position",
              variant.at("questions").at("intent").at("criteria")
                      .at("close")
                      .get<std::string>());
}

// The diff between the flat-position request and its variant must contain
// exactly the two instruction rewrites, in a stable order, each reported as
// a JSON pointer plus the new wording.
TEST(JevWordingVariantTest, FlatVariantDiffCarriesExactlyTheTwoInstructions) {
    const auto request = buildRequest(0);
    const auto variant = Trading::Tools::buildPreviousWordingVariant(request);

    const auto diff = Trading::Tools::describeWordingDiff(request, variant);

    ASSERT_EQ(2u, diff.size());
    EXPECT_EQ("/questions/bias/instructions", diff.at(0).at("pointer").get<std::string>());
    EXPECT_EQ("long or short?", diff.at(0).at("to").get<std::string>());
    EXPECT_EQ("/questions/intent/instructions",
              diff.at(1).at("pointer").get<std::string>());
    EXPECT_EQ("open, close, or hold?", diff.at(1).at("to").get<std::string>());
}

// For an occupied position the diff contains the two instruction rewrites
// plus the close-criteria rewrite at /questions/intent/criteria/close.
TEST(JevWordingVariantTest, OccupiedVariantDiffCarriesTheCloseCriteriaEntry) {
    const auto request = buildRequest(2);
    const auto variant = Trading::Tools::buildPreviousWordingVariant(request);

    const auto diff = Trading::Tools::describeWordingDiff(request, variant);

    ASSERT_EQ(3u, diff.size());
    EXPECT_EQ("/questions/intent/criteria/close",
              diff.at(1).at("pointer").get<std::string>());
    EXPECT_EQ("reduce the current position", diff.at(1).at("to").get<std::string>());
}

// Comparing a document with itself produces an empty diff.
TEST(JevWordingDiffTest, IdenticalDocumentsProduceAnEmptyDiff) {
    const auto document = Json{{"a", 1}};

    const auto diff = Trading::Tools::describeWordingDiff(document, document);

    EXPECT_TRUE(diff.empty());
}

// A differing leaf value is reported as one entry with the JSON pointer of
// the leaf and the old (from) and new (to) values.
TEST(JevWordingDiffTest, LeafDifferenceIsReportedAsFromTo) {
    const auto before = Json{{"a", 1}};
    const auto after = Json{{"a", 2}};

    const auto diff = Trading::Tools::describeWordingDiff(before, after);

    ASSERT_EQ(1u, diff.size());
    EXPECT_EQ("/a", diff.at(0).at("pointer").get<std::string>());
    EXPECT_EQ(1, diff.at(0).at("from"));
    EXPECT_EQ(2, diff.at(0).at("to"));
}

// Documents whose key sets differ (a key exists only on one side) cannot be
// described value-by-value and are reported as a "key set differs" error.
TEST(JevWordingDiffTest, KeySetDifferenceIsReportedAsAnError) {
    const auto before = Json{{"a", 1}};
    const auto after = Json{{"a", 1}, {"b", 2}};

    const auto diff = Trading::Tools::describeWordingDiff(before, after);

    ASSERT_EQ(1u, diff.size());
    EXPECT_EQ("key set differs", diff.at(0).at("error").get<std::string>());
}

// Arrays of different length at the same pointer are reported as an "array
// length differs" error at that pointer.
TEST(JevWordingDiffTest, ArrayLengthDifferenceIsReportedAsAnError) {
    const auto before = Json{{"a", Json::array({1, 2})}};
    const auto after = Json{{"a", Json::array({1})}};

    const auto diff = Trading::Tools::describeWordingDiff(before, after);

    ASSERT_EQ(1u, diff.size());
    EXPECT_EQ("/a", diff.at(0).at("pointer").get<std::string>());
    EXPECT_EQ("array length differs", diff.at(0).at("error").get<std::string>());
}

}  // namespace
