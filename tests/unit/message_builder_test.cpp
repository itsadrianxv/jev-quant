#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>

#include "common/async_logger.h"

namespace {

// Constructing a LogWriter over fresh zeroed storage must leave the buffer
// as an empty (NUL-terminated) string, not garbage.
TEST(LogWriterTest, FreshWriterLeavesStorageAnEmptyString) {
    std::array<char, 32> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    EXPECT_EQ("", std::string_view(storage.data()));
}

// Chained append calls build up a line in place: literal text, a signed
// integer, and an unsigned size_t are all rendered correctly.
TEST(LogWriterTest, ChainedAppendsFormatTextAndIntegers) {
    std::array<char, 32> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("event=test id=").append(42).append(" seq=")
            .append(static_cast<std::size_t>(7));

    EXPECT_EQ("event=test id=42 seq=7", std::string_view(storage.data()));
}

// With a zero-capacity buffer every append is a no-op and the storage stays
// empty instead of overflowing.
TEST(LogWriterTest, ZeroCapacityBufferDropsEveryAppend) {
    std::array<char, 1> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("ignored").append(1);

    EXPECT_EQ("", std::string_view(storage.data()));
}

// Text longer than the buffer is truncated at capacity and remains a valid
// NUL-terminated string (7 chars + NUL in an 8-byte buffer).
TEST(LogWriterTest, OversizedTextIsTruncatedAtCapacity) {
    std::array<char, 8> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("event=truncated_message");

    EXPECT_EQ("event=t", std::string_view(storage.data()));
}

// Once truncation happens the writer is spent: later appends are dropped so
// they cannot overwrite the NUL terminator.
TEST(LogWriterTest, AppendsAfterTruncationAreDropped) {
    std::array<char, 8> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("event=truncated_message").append(123456);

    EXPECT_EQ("event=t", std::string_view(storage.data()));
}

}  // namespace
