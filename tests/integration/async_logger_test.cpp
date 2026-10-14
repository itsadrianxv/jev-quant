#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/async_logger.h"

namespace {

using Common::AsyncLogger;
using Common::LogLevel;
using namespace std::chrono_literals;

class AsyncLoggerContractTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto pattern = (std::filesystem::temp_directory_path() /
                        "jev-async-logger-XXXXXX").string();
        ASSERT_NE(nullptr, ::mkdtemp(pattern.data()));
        output_dir_ = pattern;
    }

    void TearDown() override {
        if (!output_dir_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(output_dir_, error);
            EXPECT_FALSE(error) << error.message();
        }
    }

    // Read only after stop/destruction has joined the consumer and closed files.
    // Check timestamp syntax without asserting elapsed time or a wall-clock value.
    auto readRecords(const std::string& name = "events.log") -> std::vector<std::string> {
        std::ifstream file(output_dir_ / name);
        EXPECT_TRUE(file.is_open()) << name;
        std::vector<std::string> records;
        for (std::string line; std::getline(file, line);) {
            const auto separator = line.find(' ');
            EXPECT_NE(std::string::npos, separator) << line;
            if (separator == std::string::npos) {
                continue;
            }
            std::int64_t timestamp = 0;
            const auto* end = line.data() + separator;
            const auto result = std::from_chars(line.data(), end, timestamp);
            EXPECT_EQ(std::errc{}, result.ec) << line;
            EXPECT_EQ(end, result.ptr) << line;
            EXPECT_GT(timestamp, 0) << line;
            records.push_back(line.substr(separator + 1));
        }
        EXPECT_TRUE(file.eof()) << name;
        return records;
    }

    std::filesystem::path output_dir_;
};

TEST_F(AsyncLoggerContractTest, RejectsZeroQueueCapacity) {
    EXPECT_THROW((AsyncLogger{output_dir_, 0, 1ms}), std::invalid_argument);
}

TEST_F(AsyncLoggerContractTest, RejectsNegativePollInterval) {
    EXPECT_THROW((AsyncLogger{output_dir_, 1, -1ms}), std::invalid_argument);
}

TEST_F(AsyncLoggerContractTest, AcceptsZeroPollInterval) {
    AsyncLogger logger(output_dir_, 1, 0ms);
    auto producer = logger.registerProducer("Test", "events.log");
    producer.log(LogLevel::INFO, "accepted");
    logger.start();
    logger.stop();

    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] accepted"}), readRecords());
}

TEST_F(AsyncLoggerContractTest, RejectsEmptyAndOverlongComponentNames) {
    AsyncLogger logger(output_dir_, 1, 1ms);

    EXPECT_THROW(logger.registerProducer("", "empty.log"), std::invalid_argument);
    EXPECT_THROW(logger.registerProducer(std::string(AsyncLogger::kComponentCapacity, 'c'),
                                         "long.log"), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(output_dir_ / "empty.log"));
    EXPECT_FALSE(std::filesystem::exists(output_dir_ / "long.log"));
}

TEST_F(AsyncLoggerContractTest, PreservesMaximumLengthComponentName) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    const std::string component(AsyncLogger::kComponentCapacity - 1, 'c');
    auto producer = logger.registerProducer(component, "events.log");
    producer.log(LogLevel::INFO, "accepted");
    logger.start();
    logger.stop();

    EXPECT_EQ((std::vector<std::string>{"[INFO] [" + component + "] accepted"}),
              readRecords());
}

TEST_F(AsyncLoggerContractTest, FailedFileOpenDoesNotPreventLaterRegistration) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    ASSERT_TRUE(std::filesystem::create_directory(output_dir_ / "blocked"));

    // Opening a directory as a file fails regardless of the caller's privileges.
    EXPECT_THROW(logger.registerProducer("Blocked", "blocked"), std::runtime_error);
    auto producer = logger.registerProducer("Test", "events.log");
    producer.log(LogLevel::INFO, "accepted");
    logger.start();
    logger.stop();

    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] accepted"}), readRecords());
}

TEST_F(AsyncLoggerContractTest, StopDrainsEveryQueuedRecordInProducerOrder) {
    constexpr int kRecordCount = 8;
    AsyncLogger logger(output_dir_, kRecordCount, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    std::vector<std::string> expected;
    for (int i = 0; i < kRecordCount; ++i) {
        producer.logFixed(LogLevel::INFO, [i](auto& out) { out.append(i); });
        expected.push_back("[INFO] [Test] " + std::to_string(i));
    }
    logger.start();
    logger.stop();

    EXPECT_EQ(expected, readRecords());
}

TEST_F(AsyncLoggerContractTest, LogPreservesEmptyAndExactFitMessagesAndTruncatesOversizedOnes) {
    AsyncLogger logger(output_dir_, 5, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    const auto capacity = AsyncLogger::kMessageCapacity;
    const std::vector<std::size_t> lengths{0, capacity - 2, capacity - 1,
                                         capacity, capacity + 32};
    std::vector<std::string> expected;
    for (const auto length : lengths) {
        producer.log(LogLevel::INFO, std::string(length, 'x'));
        expected.push_back("[INFO] [Test] " +
                           std::string(length < capacity ? length : capacity - 1, 'x'));
    }
    logger.start();
    logger.stop();

    EXPECT_EQ(expected, readRecords());
}

TEST_F(AsyncLoggerContractTest, LogFixedPreservesEmptyAndExactFitMessagesAndTruncatesOversizedOnes) {
    AsyncLogger logger(output_dir_, 5, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    const auto capacity = AsyncLogger::kMessageCapacity;
    const std::vector<std::size_t> lengths{0, capacity - 2, capacity - 1,
                                         capacity, capacity + 32};
    std::vector<std::string> expected;
    for (const auto length : lengths) {
        const std::string message(length, 'x');
        producer.logFixed(LogLevel::WARN, [&](auto& out) { out.append(message); });
        expected.push_back("[WARN] [Test] " +
                           std::string(length < capacity ? length : capacity - 1, 'x'));
    }
    logger.start();
    logger.stop();

    EXPECT_EQ(expected, readRecords());
}

TEST_F(AsyncLoggerContractTest, FullQueueSkipsFormatterAndPreservesAcceptedRecord) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    producer.log(LogLevel::INFO, "accepted");
    bool formatter_called = false;
    producer.logFixed(LogLevel::ERROR, [&](auto& out) {
        formatter_called = true;
        out.append("dropped");
    });
    logger.start();
    logger.stop();

    EXPECT_FALSE(formatter_called);
    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] accepted"}), readRecords());
}

TEST_F(AsyncLoggerContractTest, DestructorDrainsAllProducerQueues) {
    const std::vector<std::string> expected{"[INFO] [Test] first", "[INFO] [Test] second"};
    {
        AsyncLogger logger(output_dir_, 2, 1ms);
        auto first = logger.registerProducer("Test", "first.log");
        auto second = logger.registerProducer("Test", "second.log");
        for (auto* producer : {&first, &second}) {
            producer->log(LogLevel::INFO, "first");
            producer->log(LogLevel::INFO, "second");
        }
        logger.start();
    }

    EXPECT_EQ(expected, readRecords("first.log"));
    EXPECT_EQ(expected, readRecords("second.log"));
}

TEST_F(AsyncLoggerContractTest, RepeatedStartWhileRunningAndRepeatedStopAreNoOps) {
    AsyncLogger logger(output_dir_, 2, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    producer.log(LogLevel::INFO, "before start");
    logger.start();
    logger.start();
    producer.log(LogLevel::INFO, "after repeated start");
    logger.stop();
    logger.stop();

    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] before start",
                                       "[INFO] [Test] after repeated start"}), readRecords());
}

TEST_F(AsyncLoggerContractTest, StopBeforeStartPreservesQueuedRecordsAndAllowsRegistration) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    auto first = logger.registerProducer("Test", "first.log");
    first.log(LogLevel::INFO, "queued");
    logger.stop();
    logger.stop();
    auto second = logger.registerProducer("Test", "second.log");
    second.log(LogLevel::INFO, "registered after stop");
    logger.start();
    logger.stop();

    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] queued"}), readRecords("first.log"));
    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] registered after stop"}),
              readRecords("second.log"));
}

TEST_F(AsyncLoggerContractTest, RejectsRestartAfterStopAndPreservesOutput) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    producer.log(LogLevel::INFO, "accepted");
    logger.start();
    logger.stop();

    EXPECT_THROW(logger.start(), std::logic_error);
    EXPECT_THROW(logger.start(), std::logic_error);
    logger.stop();
    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] accepted"}), readRecords());
}

TEST_F(AsyncLoggerContractTest, RejectsProducerRegistrationAfterStop) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    logger.start();
    logger.stop();

    EXPECT_THROW(logger.registerProducer("Late", "late.log"), std::logic_error);
    EXPECT_FALSE(std::filesystem::exists(output_dir_ / "late.log"));
}

TEST_F(AsyncLoggerContractTest, FirstLogBindsOwnerAndIgnoresForeignThreadWrites) {
    AsyncLogger logger(output_dir_, 2, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    producer.log(LogLevel::INFO, "first");
    bool formatter_called = false;
    // Thread creation and join order handle access; no simultaneous rebinding.
    std::jthread foreign([&] {
        producer.bindToCurrentThread();
        producer.log(LogLevel::ERROR, "foreign");
        producer.logFixed(LogLevel::ERROR, [&](auto&) { formatter_called = true; });
    });
    foreign.join();
    producer.log(LogLevel::INFO, "second");
    logger.start();
    logger.stop();

    EXPECT_FALSE(formatter_called);
    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] first", "[INFO] [Test] second"}),
              readRecords());
}

TEST_F(AsyncLoggerContractTest, FirstLogFixedBindsOwnerAndIgnoresForeignThreadWrites) {
    AsyncLogger logger(output_dir_, 2, 1ms);
    auto producer = logger.registerProducer("Test", "events.log");
    producer.logFixed(LogLevel::INFO, [](auto& out) { out.append("first"); });
    bool formatter_called = false;
    std::jthread foreign([&] {
        producer.log(LogLevel::ERROR, "foreign");
        producer.logFixed(LogLevel::ERROR, [&](auto&) { formatter_called = true; });
    });
    foreign.join();
    producer.logFixed(LogLevel::INFO, [](auto& out) { out.append("second"); });
    logger.start();
    logger.stop();

    EXPECT_FALSE(formatter_called);
    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] first", "[INFO] [Test] second"}),
              readRecords());
}

TEST_F(AsyncLoggerContractTest, MoveConstructionLeavesSourceInertAndUnboundHandleUsableOnWorker) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    auto source = logger.registerProducer("Test", "events.log");
    auto destination = std::move(source);
    bool formatter_called = false;
    source.bindToCurrentThread();
    source.log(LogLevel::ERROR, "moved source");
    source.logFixed(LogLevel::ERROR, [&](auto&) { formatter_called = true; });
    std::jthread worker([&] { destination.log(LogLevel::INFO, "worker"); });
    worker.join();
    logger.start();
    logger.stop();

    EXPECT_FALSE(formatter_called);
    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] worker"}), readRecords());
}

TEST_F(AsyncLoggerContractTest, MovingBoundHandlePreservesItsOriginalThreadOwner) {
    AsyncLogger logger(output_dir_, 1, 1ms);
    auto source = logger.registerProducer("Test", "events.log");
    source.bindToCurrentThread();
    auto destination = std::move(source);
    std::jthread foreign([&] {
        destination.bindToCurrentThread();
        destination.log(LogLevel::ERROR, "foreign");
    });
    foreign.join();
    destination.log(LogLevel::INFO, "original owner");
    logger.start();
    logger.stop();

    EXPECT_EQ((std::vector<std::string>{"[INFO] [Test] original owner"}), readRecords());
}

TEST_F(AsyncLoggerContractTest, MoveAssignmentTransfersQueueAndLeavesSourceInert) {
    AsyncLogger logger(output_dir_, 2, 1ms);
    auto source = logger.registerProducer("Source", "source.log");
    auto destination = logger.registerProducer("Destination", "destination.log");
    source.log(LogLevel::INFO, "before move");
    std::jthread previous_owner([&] {
        destination.log(LogLevel::WARN, "before assignment");
    });
    previous_owner.join();

    destination = std::move(source);
    bool formatter_called = false;
    source.bindToCurrentThread();
    source.log(LogLevel::ERROR, "moved source");
    source.logFixed(LogLevel::ERROR, [&](auto&) { formatter_called = true; });
    std::jthread foreign([&] {
        destination.bindToCurrentThread();
        destination.log(LogLevel::ERROR, "foreign");
    });
    foreign.join();
    destination.logFixed(LogLevel::INFO, [](auto& out) { out.append("after move"); });
    logger.start();
    logger.stop();

    EXPECT_FALSE(formatter_called);
    EXPECT_EQ((std::vector<std::string>{"[INFO] [Source] before move",
                                       "[INFO] [Source] after move"}), readRecords("source.log"));
    EXPECT_EQ((std::vector<std::string>{"[WARN] [Destination] before assignment"}),
              readRecords("destination.log"));
}

}  // namespace
