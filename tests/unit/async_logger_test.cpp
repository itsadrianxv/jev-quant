#include <gtest/gtest.h>

#include "common/async_logger.h"

namespace {

TEST(LogLevelNameTest, MapsEveryLevelToItsRecordLabel) {
    EXPECT_EQ("DEBUG", Common::logLevelName(Common::LogLevel::DEBUG));
    EXPECT_EQ("INFO", Common::logLevelName(Common::LogLevel::INFO));
    EXPECT_EQ("WARN", Common::logLevelName(Common::LogLevel::WARN));
    EXPECT_EQ("ERROR", Common::logLevelName(Common::LogLevel::ERROR));
}

TEST(LogLevelNameTest, UnrecognizedLevelUsesUnknownLabel) {
    EXPECT_EQ("UNKNOWN", Common::logLevelName(static_cast<Common::LogLevel>(255)));
}

}  // namespace
