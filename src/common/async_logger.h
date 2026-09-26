#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/lf_queue.h"

namespace Common {

enum class LogLevel : std::uint8_t {
    DEBUG,
    INFO,
    WARN,
    ERROR,
};

[[nodiscard]] inline auto logLevelName(LogLevel level) noexcept
        -> std::string_view {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERROR:
            return "ERROR";
    }
    return "UNKNOWN";
}

class AsyncLogger final {
  public:
    static constexpr std::size_t kComponentCapacity = 64;
    static constexpr std::size_t kMessageCapacity = 1024;

    struct LogRecord {
        std::int64_t timestamp_us = 0;
        LogLevel level = LogLevel::INFO;
        std::array<char, kComponentCapacity> component{};
        std::array<char, kMessageCapacity> message{};
    };

  private:
    struct ProducerState {
        ProducerState(std::string component_name, std::filesystem::path path,
                                    std::size_t queue_capacity)
                : component(std::move(component_name)), queue(queue_capacity) {
            if (component.empty() || component.size() >= kComponentCapacity) {
                throw std::invalid_argument("Invalid logger component name");
            }
            file.open(path, std::ios::out | std::ios::trunc);
            if (!file.is_open()) {
                throw std::runtime_error("Could not open logger output file: " +
                                                                  path.string());
            }
        }

        std::string component;
        LFQueue<LogRecord> queue;
        std::ofstream file;
        std::atomic<bool> bound{false};
    };

  public:
    class ProducerHandle final {
      public:
        ProducerHandle() = delete;
        ProducerHandle(const ProducerHandle&) = delete;
        ProducerHandle& operator=(const ProducerHandle&) = delete;
        ProducerHandle(ProducerHandle&& other) noexcept
                : state_(std::exchange(other.state_, nullptr)),
                    owner_(std::exchange(other.owner_, std::thread::id{})),
                    owner_logger_(std::exchange(other.owner_logger_, nullptr)) {}
        ProducerHandle& operator=(ProducerHandle&& other) noexcept {
            if (this != &other) {
                state_ = std::exchange(other.state_, nullptr);
                owner_ = std::exchange(other.owner_, std::thread::id{});
                owner_logger_ = std::exchange(other.owner_logger_, nullptr);
            }
            return *this;
        }

        auto log(LogLevel level, std::string_view message) noexcept -> void {
            if (state_ == nullptr) {
                return;
            }
            if (owner_ == std::thread::id{}) {
                bindToCurrentThread();
            }
            if (owner_ != std::this_thread::get_id()) return;
            auto* slot = state_->queue.tryGetNextToWriteTo();
            if (slot == nullptr) {
                return;
            }

            LogRecord record;
            record.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                                                std::chrono::system_clock::now().time_since_epoch())
                                                                .count();
            record.level = level;
            copyText(record.component, state_->component);
            copyText(record.message, message);
            *slot = record;
            state_->queue.updateWriteIndex();
        }

        auto bindToCurrentThread() noexcept -> void {
            if (state_ == nullptr || owner_ != std::thread::id{}) {
                return;
            }
            owner_ = std::this_thread::get_id();
            state_->bound.store(true, std::memory_order_release);
            if (owner_logger_ != nullptr) {
                owner_logger_->binding_cv_.notify_all();
            }
        }

      private:
        friend class AsyncLogger;

        explicit ProducerHandle(ProducerState* state, AsyncLogger* owner_logger)
                : state_(state), owner_logger_(owner_logger) {}

        template <std::size_t N>
        static auto copyText(std::array<char, N>& destination,
                                                  std::string_view source) noexcept -> void {
            const auto count = source.size() < N - 1 ? source.size() : N - 1;
            for (std::size_t i = 0; i < count; ++i) {
                destination[i] = source[i];
            }
            destination[count] = '\0';
        }

        ProducerState* state_ = nullptr;
        std::thread::id owner_;
        AsyncLogger* owner_logger_ = nullptr;
    };

    AsyncLogger(std::filesystem::path output_directory,
                            std::size_t queue_capacity,
                            std::chrono::milliseconds poll_interval)
            : output_directory_(std::move(output_directory)),
                queue_capacity_(queue_capacity),
                poll_interval_(poll_interval) {
        if (queue_capacity_ == 0 || poll_interval_.count() < 0) {
            throw std::invalid_argument("Invalid logger configuration");
        }
        std::filesystem::create_directories(output_directory_);
    }

    ~AsyncLogger() { stop(); }

    AsyncLogger() = delete;
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    auto registerProducer(std::string component, std::string file_name)
            -> ProducerHandle {
        std::lock_guard lock(mutex_);
        if (started_) {
            throw std::logic_error("Cannot register logger producer after start");
        }
        auto path = output_directory_ / std::move(file_name);
        auto state = std::make_unique<ProducerState>(
                std::move(component), std::move(path), queue_capacity_);
        auto* state_ptr = state.get();
        producers_.push_back(std::move(state));
        return ProducerHandle(state_ptr, this);
    }

    auto waitForProducerBindings(std::size_t expected_producers,
                                 std::chrono::milliseconds timeout) -> void {
        std::unique_lock lock(mutex_);
        binding_cv_.wait_for(lock, timeout, [this, expected_producers] {
            if (producers_.size() != expected_producers) {
                return false;
            }
            for (const auto& producer : producers_) {
                if (!producer->bound.load(std::memory_order_acquire)) {
                    return false;
                }
            }
            return true;
        });
    }

    auto start() -> void {
        std::lock_guard lock(mutex_);
        if (started_) {
            return;
        }
        stopped_ = false;
        started_ = true;
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { run(); });
    }

    auto stop() noexcept -> void {
        {
            std::lock_guard lock(mutex_);
            if (!started_ || stopped_) {
                return;
            }
            stopped_ = true;
            running_.store(false, std::memory_order_release);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        for (auto& producer : producers_) {
            producer->file.close();
        }
        started_ = false;
    }

  private:
    auto run() noexcept -> void {
        while (running_.load(std::memory_order_acquire)) {
            drain();
            std::this_thread::sleep_for(poll_interval_);
        }
        do {
            drain();
        } while (hasPendingRecords());
    }

    auto drain() noexcept -> void {
        for (auto& producer : producers_) {
            while (const auto* record = producer->queue.getNextToRead()) {
                producer->file << record->timestamp_us << " ["
                                              << logLevelName(record->level) << "] ["
                                              << record->component.data() << "] "
                                              << record->message.data() << '\n';
                producer->queue.updateReadIndex();
            }
            producer->file.flush();
        }
    }

    [[nodiscard]] auto hasPendingRecords() const noexcept -> bool {
        for (const auto& producer : producers_) {
            if (producer->queue.size() != 0) {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path output_directory_;
    std::size_t queue_capacity_;
    std::chrono::milliseconds poll_interval_;
    std::vector<std::unique_ptr<ProducerState>> producers_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::condition_variable binding_cv_;
    bool started_ = false;
    bool stopped_ = false;
};

}  // namespace Common
