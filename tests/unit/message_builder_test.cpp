#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "common/async_logger.h"

namespace {

using MessageBuilder = Common::AsyncLogger::ProducerHandle::MessageBuilder;

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

// A one-byte buffer reserves its only byte for the NUL terminator.
TEST(MessageBuilderTest, OneByteBufferDropsEveryAppend) {
    std::array<char, 1> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append("ignored").append(1);

    EXPECT_EQ("", std::string_view(storage.data()));
}

TEST(MessageBuilderTest, ZeroCapacityBufferNeverTouchesStorage) {
    char sentinel = '!';
    MessageBuilder builder(&sentinel, 0);
    EXPECT_EQ('!', sentinel);

    builder.append("ignored").append(42).append("");

    EXPECT_EQ('!', sentinel);
}

TEST(MessageBuilderTest, ZeroCapacityAcceptsNullStorage) {
    MessageBuilder builder(nullptr, 0);

    EXPECT_EQ(&builder, &builder.append("ignored").append(-1));
}

TEST(MessageBuilderTest, EmptyAppendPreservesExistingText) {
    std::array<char, 8> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append("").append("text").append("");

    EXPECT_EQ("text", std::string_view(storage.data()));
}

TEST(MessageBuilderTest, ExactFitPreservesTerminatorAndAdjacentBytes) {
    std::array<char, 10> storage;
    storage.fill('!');
    MessageBuilder builder(storage.data() + 1, 8);

    builder.append("abc").append("defg").append("ignored");

    EXPECT_EQ("abcdefg", std::string_view(storage.data() + 1));
    EXPECT_EQ('\0', storage[8]);
    EXPECT_EQ('!', storage.front());
    EXPECT_EQ('!', storage.back());
}

TEST(MessageBuilderTest, FormatsSignedAndUnsignedIntegerLimits) {
    std::array<char, 80> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append(std::numeric_limits<std::int64_t>::min()).append(" ")
            .append(std::numeric_limits<std::int64_t>::max()).append(" ")
            .append(std::numeric_limits<std::uint64_t>::max()).append(" ")
            .append(0);

    EXPECT_EQ("-9223372036854775808 9223372036854775807 18446744073709551615 0",
              std::string_view(storage.data()));
}

TEST(MessageBuilderTest, TruncatesIntegerWhenOnlyItsPrefixFits) {
    std::array<char, 8> storage{};
    MessageBuilder builder(storage.data(), storage.size());

    builder.append("id=").append(-12345);

    EXPECT_EQ("id=-123", std::string_view(storage.data()));
    EXPECT_EQ('\0', storage.back());
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
