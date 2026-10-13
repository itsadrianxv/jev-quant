#include <gtest/gtest.h>

#include "trading/strategy/jev_decision.h"

namespace {

// A freshly created decision queue has no readable decision.
TEST(JevDecisionQueueTest, EmptyQueueHasNothingToRead) {
    Trading::JevDecisionLFQueue queue(4);

    EXPECT_EQ(nullptr, queue.getNextToRead());
}

// Writing a decision into the write slot and publishing it makes the exact
// payload readable exactly once; after the read is consumed there is
// nothing left to read.
TEST(JevDecisionQueueTest, PublishedDecisionIsReadableOnce) {
    Trading::JevDecisionLFQueue queue(4);

    auto* slot = queue.getNextToWriteTo();
    ASSERT_NE(slot, nullptr);
    slot->evaluation_id_ = 7;
    slot->ticker_id_ = 0;
    slot->bias_ = Trading::JevBias::LONG;
    slot->intent_ = Trading::JevIntent::OPEN;
    slot->long_probability_ = 0.6;
    slot->short_probability_ = 0.4;
    slot->confidence_ = 0.9;
    queue.updateWriteIndex();

    const auto* decision = queue.getNextToRead();
    ASSERT_NE(decision, nullptr);
    EXPECT_EQ(7u, decision->evaluation_id_);
    EXPECT_EQ(0u, decision->ticker_id_);
    EXPECT_EQ(Trading::JevBias::LONG, decision->bias_);
    EXPECT_EQ(Trading::JevIntent::OPEN, decision->intent_);
    EXPECT_DOUBLE_EQ(0.6, decision->long_probability_);
    EXPECT_DOUBLE_EQ(0.4, decision->short_probability_);
    EXPECT_DOUBLE_EQ(0.9, decision->confidence_);
    queue.updateReadIndex();

    EXPECT_EQ(nullptr, queue.getNextToRead());
}

}  // namespace
