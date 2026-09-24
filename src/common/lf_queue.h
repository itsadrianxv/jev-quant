#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Common {

/// A bounded single-producer/single-consumer ring queue.
///
/// The producer owns the write side and the consumer owns the read side. The
/// queue allocates all storage at construction time. A full queue is a fatal
/// local programming error for the initial framework; callers can use the
/// tryGetNextToWriteTo() helper when they need to report the condition first.
template <typename T>
class LFQueue final {
 public:
  explicit LFQueue(std::size_t num_elems)
      : store_(num_elems) {
    if (num_elems == 0) {
      throw std::invalid_argument("LFQueue capacity must be greater than zero");
    }
  }

  LFQueue() = delete;
  LFQueue(const LFQueue&) = delete;
  LFQueue& operator=(const LFQueue&) = delete;
  LFQueue(LFQueue&&) = delete;
  LFQueue& operator=(LFQueue&&) = delete;

  /// Return the next writable slot, or nullptr when the queue is full.
  T* tryGetNextToWriteTo() noexcept {
    if (isFull()) {
      return nullptr;
    }
    return &store_[next_write_index_.load(std::memory_order_relaxed)];
  }

  /// Return the next writable slot. The caller must have capacity available.
  T* getNextToWriteTo() noexcept {
    auto* slot = tryGetNextToWriteTo();
    if (slot == nullptr) {
      std::terminate();
    }
    return slot;
  }

  auto updateWriteIndex() noexcept -> void {
    if (isFull()) {
      std::terminate();
    }

    const auto next_index =
        (next_write_index_.load(std::memory_order_relaxed) + 1) % store_.size();
    next_write_index_.store(next_index, std::memory_order_release);
    num_elements_.fetch_add(1, std::memory_order_release);
  }

  /// Return the next readable slot, or nullptr when the queue is empty.
  const T* getNextToRead() const noexcept {
    if (size() == 0) {
      return nullptr;
    }
    return &store_[next_read_index_.load(std::memory_order_relaxed)];
  }

  auto updateReadIndex() noexcept -> void {
    if (size() == 0) {
      std::terminate();
    }

    const auto next_index =
        (next_read_index_.load(std::memory_order_relaxed) + 1) % store_.size();
    next_read_index_.store(next_index, std::memory_order_release);
    num_elements_.fetch_sub(1, std::memory_order_release);
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return num_elements_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return store_.size();
  }

  [[nodiscard]] auto isFull() const noexcept -> bool {
    return size() == capacity();
  }

 private:
  std::vector<T> store_;
  std::atomic<std::size_t> next_write_index_{0};
  std::atomic<std::size_t> next_read_index_{0};
  std::atomic<std::size_t> num_elements_{0};
};

}  // namespace Common
