#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>

#include "common/async_logger.h"

namespace {

TEST(LogWriterTest, FreshWriterLeavesStorageAnEmptyString) {
    std::array<char, 32> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    EXPECT_EQ("", std::string_view(storage.data()));
}

TEST(LogWriterTest, ChainedAppendsFormatTextAndIntegers) {
    std::array<char, 32> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("event=test id=").append(42).append(" seq=")
            .append(static_cast<std::size_t>(7));

    EXPECT_EQ("event=test id=42 seq=7", std::string_view(storage.data()));
}

TEST(LogWriterTest, ZeroCapacityBufferDropsEveryAppend) {
    std::array<char, 1> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("ignored").append(1);

    EXPECT_EQ("", std::string_view(storage.data()));
}

TEST(LogWriterTest, OversizedTextIsTruncatedAtCapacity) {
    std::array<char, 8> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("event=truncated_message");

    EXPECT_EQ("event=t", std::string_view(storage.data()));
}

TEST(LogWriterTest, AppendsAfterTruncationAreDropped) {
    std::array<char, 8> storage{};
    Common::LogWriter writer(storage.data(), storage.size());

    writer.append("event=truncated_message").append(123456);

    EXPECT_EQ("event=t", std::string_view(storage.data()));
}

}  // namespace
