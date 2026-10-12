#include <gtest/gtest.h>

#include "common/lf_queue.h"

namespace {

TEST(LFQueueTest, EmptyQueueHasNoReadableSlot) {
    Common::LFQueue<int> queue(2);

    EXPECT_EQ(0u, queue.size());
    EXPECT_EQ(2u, queue.capacity());
    EXPECT_FALSE(queue.isFull());
    EXPECT_EQ(nullptr, queue.getNextToRead());
}

TEST(LFQueueTest, WrittenSlotsAreReadableInFifoOrder) {
    Common::LFQueue<int> queue(2);

    int* first = queue.tryGetNextToWriteTo();
    ASSERT_NE(nullptr, first);
    *first = 11;
    queue.updateWriteIndex();

    int* second = queue.tryGetNextToWriteTo();
    ASSERT_NE(nullptr, second);
    *second = 22;
    queue.updateWriteIndex();

    EXPECT_EQ(2u, queue.size());

    const int* read_first = queue.getNextToRead();
    ASSERT_NE(nullptr, read_first);
    EXPECT_EQ(11, *read_first);
    queue.updateReadIndex();

    const int* read_second = queue.getNextToRead();
    ASSERT_NE(nullptr, read_second);
    EXPECT_EQ(22, *read_second);
    queue.updateReadIndex();

    EXPECT_EQ(0u, queue.size());
}

TEST(LFQueueTest, FullQueueRejectsNewWriteSlots) {
    Common::LFQueue<int> queue(2);

    *queue.tryGetNextToWriteTo() = 11;
    queue.updateWriteIndex();
    *queue.tryGetNextToWriteTo() = 22;
    queue.updateWriteIndex();

    EXPECT_TRUE(queue.isFull());
    EXPECT_EQ(nullptr, queue.tryGetNextToWriteTo());
}

TEST(LFQueueTest, ConsumedSlotsFreeCapacityForNewWrites) {
    Common::LFQueue<int> queue(1);

    *queue.tryGetNextToWriteTo() = 7;
    queue.updateWriteIndex();
    EXPECT_EQ(nullptr, queue.tryGetNextToWriteTo());

    EXPECT_EQ(7, *queue.getNextToRead());
    queue.updateReadIndex();

    int* freed = queue.tryGetNextToWriteTo();
    ASSERT_NE(nullptr, freed);
    *freed = 8;
    queue.updateWriteIndex();

    EXPECT_EQ(8, *queue.getNextToRead());
}

TEST(LFQueueTest, ZeroCapacityConstructionThrowsInvalidArgument) {
    EXPECT_THROW(Common::LFQueue<int> queue(0), std::invalid_argument);
}

}  // namespace
