#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>

#include "common/async_logger.h"

namespace {

using Common::AsyncLogger::ProducerHandle::MessageBuilder;

// Constructing a MessageBuilder over fresh zeroed storage must leave the
// buffer as an empty (NUL-terminated) string, not garbage.
TEST(MessageBuilderTest, FreshBuilderLeavesStorageAnEmptyString) {
    std::array<char, 32> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    EXPECT_EQ("", std::string_view(storage.data()));
}

// Chained append calls build up a line in place: literal text, a signed
// integer, and an unsigned size_t are all rendered correctly.
TEST(MessageBuilderTest, ChainedAppendsFormatTextAndIntegers) {
    std::array<char, 32> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append("event=test id=").append(42).append(" seq=")
            .append(static_cast<std::size_t>(7));

    EXPECT_EQ("event=test id=42 seq=7", std::string_view(storage.data()));
}

// With a zero-capacity buffer every append is a no-op and the storage stays
// empty instead of overflowing.
TEST(MessageBuilderTest, ZeroCapacityBufferDropsEveryAppend) {
    std::array<char, 1> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append("ignored").append(1);

    EXPECT_EQ("", std::string_view(storage.data()));
}

// Text longer than the buffer is truncated at capacity and remains a valid
// NUL-terminated string (7 chars + NUL in an 8-byte buffer).
TEST(MessageBuilderTest, OversizedTextIsTruncatedAtCapacity) {
    std::array<char, 8> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append("event=truncated_message");

    EXPECT_EQ("event=t", std::string_view(storage.data()));
}

// Once truncation happens the builder is spent: later appends are dropped so
// they cannot overwrite the NUL terminator.
TEST(MessageBuilderTest, AppendsAfterTruncationAreDropped) {
    std::array<char, 8> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append("event=truncated_message").append(123456);

    EXPECT_EQ("event=t", std::string_view(storage.data()));
}

}  // namespace
