#include <gtest/gtest.h>

#include "common/lf_queue.h"

namespace {

// A freshly created queue reports size 0, its requested capacity, not full,
// and no readable slot.
TEST(LFQueueTest, EmptyQueueHasNoReadableSlot) {
    Common::LFQueue<int> queue(2);

    EXPECT_EQ(0u, queue.size());
    EXPECT_EQ(2u, queue.capacity());
    EXPECT_FALSE(queue.isFull());
    EXPECT_EQ(nullptr, queue.getNextToRead());
}

// The write/read handshake: each written slot (obtain write pointer, store,
// updateWriteIndex) becomes readable in FIFO order via getNextToRead, and
// updateReadIndex drains the queue back to size 0.
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

// After filling every slot the queue reports full and refuses to hand out a
// new write slot (nullptr) instead of wrapping onto unread data.
TEST(LFQueueTest, FullQueueRejectsNewWriteSlots) {
    Common::LFQueue<int> queue(2);

    *queue.tryGetNextToWriteTo() = 11;
    queue.updateWriteIndex();
    *queue.tryGetNextToWriteTo() = 22;
    queue.updateWriteIndex();

    EXPECT_TRUE(queue.isFull());
    EXPECT_EQ(nullptr, queue.tryGetNextToWriteTo());
}

// Consuming a slot via updateReadIndex frees its capacity, so the producer
// can write into it again (the ring wraps on a capacity-1 queue).
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

// Constructing a queue with zero capacity is a usage error and must throw.
TEST(LFQueueTest, ZeroCapacityConstructionThrowsInvalidArgument) {
    EXPECT_THROW(Common::LFQueue<int> queue(0), std::invalid_argument);
}

}  // namespace
