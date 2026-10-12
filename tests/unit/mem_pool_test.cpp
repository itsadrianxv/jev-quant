#include <gtest/gtest.h>

#include <cstdint>

#include "common/mem_pool.h"

namespace {

// A pooled type must be trivially destructible. Uncommenting the following
// two lines must fail to compile.
// struct WithDestructor { ~WithDestructor() {} };
// Common::MemPool<WithDestructor> rejected_pool(1);

struct MemPoolProbe {
    std::uint64_t value = 0;
    bool constructed = false;
    MemPoolProbe() = default;
    explicit MemPoolProbe(std::uint64_t initial) : value(initial), constructed(true) {}
};

TEST(MemPoolTest, FreshPoolHandsOutEveryBlockAtDistinctAddresses) {
    Common::MemPool<std::uint64_t> pool(4);

    std::uint64_t* first = pool.allocate();
    std::uint64_t* second = pool.allocate();
    std::uint64_t* third = pool.allocate();
    std::uint64_t* fourth = pool.allocate();

    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    ASSERT_NE(nullptr, third);
    ASSERT_NE(nullptr, fourth);
    EXPECT_NE(first, second);
    EXPECT_NE(first, third);
    EXPECT_NE(first, fourth);
    EXPECT_NE(second, third);
    EXPECT_NE(second, fourth);
    EXPECT_NE(third, fourth);

    *first = 1;
    *second = 2;
    *third = 3;
    *fourth = 4;
    EXPECT_EQ(1u, *first);
    EXPECT_EQ(2u, *second);
    EXPECT_EQ(3u, *third);
    EXPECT_EQ(4u, *fourth);
}

TEST(MemPoolTest, DeallocatedBlocksAreReusedInLifoFreeOrder) {
    Common::MemPool<std::uint64_t> pool(4);

    std::uint64_t* first = pool.allocate();
    std::uint64_t* second = pool.allocate();
    std::uint64_t* third = pool.allocate();
    std::uint64_t* fourth = pool.allocate();
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    ASSERT_NE(nullptr, third);
    ASSERT_NE(nullptr, fourth);

    pool.deallocate(fourth);
    pool.deallocate(third);

    EXPECT_EQ(fourth, pool.allocate());
    EXPECT_EQ(third, pool.allocate());
}

TEST(MemPoolTest, RevivedBlockStartsValueInitialized) {
    Common::MemPool<std::uint64_t> pool(4);

    std::uint64_t* first = pool.allocate();
    std::uint64_t* second = pool.allocate();
    std::uint64_t* third = pool.allocate();
    std::uint64_t* fourth = pool.allocate();
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    ASSERT_NE(nullptr, third);
    ASSERT_NE(nullptr, fourth);
    pool.deallocate(fourth);

    std::uint64_t* revived = pool.allocate();

    EXPECT_EQ(fourth, revived);
    EXPECT_EQ(0u, *revived);
    *revived = 5;
    EXPECT_EQ(5u, *revived);
}

TEST(MemPoolTest, ZeroCapacityConstructionThrowsInvalidArgument) {
    EXPECT_THROW(Common::MemPool<std::uint64_t> invalid(0), std::invalid_argument);
}

TEST(MemPoolPlacementTest, AllocateForwardsArgumentsToPlacementNew) {
    Common::MemPool<MemPoolProbe> pool(2);

    MemPoolProbe* first = pool.allocate(11);

    ASSERT_NE(nullptr, first);
    EXPECT_EQ(11u, first->value);
    EXPECT_TRUE(first->constructed);

    MemPoolProbe* second = pool.allocate(22);

    EXPECT_NE(first, second);
    EXPECT_EQ(22u, second->value);
}

TEST(MemPoolPlacementTest, ReallocatedBlockStartsFromDefaultValue) {
    Common::MemPool<MemPoolProbe> pool(2);

    MemPoolProbe* first = pool.allocate(11);
    MemPoolProbe* second = pool.allocate(22);
    ASSERT_NE(nullptr, first);
    ASSERT_NE(nullptr, second);
    pool.deallocate(first);

    MemPoolProbe* reused = pool.allocate();

    EXPECT_EQ(first, reused);
    EXPECT_EQ(0u, reused->value);
    EXPECT_FALSE(reused->constructed);
}

}  // namespace
