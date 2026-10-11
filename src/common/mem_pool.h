#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/macros.h"

namespace Common {

/// A fixed-capacity pool of T objects, following the reference
/// implementation in "Building Low-Latency Applications with C++",
/// chapter 12 (common/mem_pool.h).
///
/// The pool allocates all storage once at construction time. allocate()
/// placement-news the object into the next free block and deallocate() only
/// marks the block free: the destructor is never called, so T must be
/// trivially destructible. The pool is not thread-safe; each pool has exactly
/// one owning thread.
///
/// Documented deviations from the reference implementation:
/// 1. Assertions are macros, so the failure message is only formatted on the
///    failure path. The reference formats a std::string at every
///    allocate/deallocate call site and therefore allocates on the happy path.
/// 2. Exhaustion terminates in every build mode. The reference's optimized
///    variant compiles its out-of-space assertion out in release builds and
///    then spins forever when full.
/// 3. deallocate() takes a mutable pointer; the reference takes a const
///    pointer and writes through a const cast.
/// 4. allocate() forwards its arguments; the reference copies them.
/// 5. T must be trivially destructible (static_assert); the reference calls
///    no destructor but provides no guard.
/// 6. Exhaustion is tracked with a flag and revives on deallocation. The
///    reference terminates on the allocation that consumes the final free
///    block, because its scan cannot distinguish "no free block remains"
///    from corruption; that makes full pool utilization impossible. Here all
///    capacity blocks are usable, and the next allocate() after the pool is
///    exhausted fails fast.
template <typename T>
class MemPool final {
    static_assert(std::is_trivially_destructible_v<T>,
                  "MemPool never calls destructors; T must be trivially destructible");

  public:
    explicit MemPool(std::size_t num_elems)
            : store_(num_elems, {T(), true}) /* pre-allocation of vector storage. */ {
        if (num_elems == 0) {
            throw std::invalid_argument("MemPool capacity must be greater than zero");
        }
        ASSERT(reinterpret_cast<const ObjectBlock *>(&(store_[0].object_)) ==
                       &(store_[0]),
               "T object should be first member of ObjectBlock.");
    }

    /// Allocate a new object of type T, use placement new to initialize the
    /// object, mark the block as in-use and return the object.
    template <typename... Args>
    T *allocate(Args &&...args) noexcept {
        if (UNLIKELY(exhausted_)) {
            FATAL("Memory pool out of space.");
        }
        auto *obj_block = &(store_[next_free_index_]);
        ASSERT(obj_block->is_free_, "Expected free ObjectBlock at index:" +
                                             std::to_string(next_free_index_));
        T *ret = &(obj_block->object_);
        ret = new (ret) T(std::forward<Args>(args)...);  // placement new.
        obj_block->is_free_ = false;

        updateNextFreeIndex();

        return ret;
    }

    /// Return the object back to the pool by marking the block as free again.
    /// The destructor is not called for the object.
    auto deallocate(T *elem) noexcept {
        const auto elem_index =
                (reinterpret_cast<ObjectBlock *>(elem) - &store_[0]);
        ASSERT(elem_index >= 0 && static_cast<std::size_t>(elem_index) < store_.size(),
               "Element being deallocated does not belong to this memory pool.");
        ASSERT(!store_[elem_index].is_free_, "Expected in-use ObjectBlock at index:" +
                                                     std::to_string(elem_index));
        store_[elem_index].is_free_ = true;
        if (exhausted_) {
            exhausted_ = false;
            next_free_index_ = static_cast<std::size_t>(elem_index);
        }
    }

    // Deleted default, copy & move constructors and assignment-operators.
    MemPool() = delete;

    MemPool(const MemPool &) = delete;

    MemPool(MemPool &&) = delete;

    MemPool &operator=(const MemPool &) = delete;

    MemPool &operator=(MemPool &&) = delete;

  private:
    /// Find the next available free block to be used for the next allocation.
    /// When a complete cycle finds no free block the pool is exhausted: the
    /// flag makes the next allocate() fail fast instead of spinning, and a
    /// later deallocate() re-seeds the scan at the freed block.
    auto updateNextFreeIndex() noexcept {
        const auto initial_free_index = next_free_index_;
        do {
            if (store_[next_free_index_].is_free_) {
                return;
            }
            ++next_free_index_;
            if (UNLIKELY(next_free_index_ == store_.size())) {
                next_free_index_ = 0;
            }
        } while (next_free_index_ != initial_free_index);
        exhausted_ = true;
    }

    /// It is better to have one vector of structs with two objects than two
    /// vectors of one object. Consider how these are accessed and cache
    /// performance.
    struct ObjectBlock {
        T object_;
        bool is_free_ = true;
    };

    /// We could've chosen to use a std::array that would allocate the memory
    /// on the stack instead of the heap. We would have to measure to see
    /// which one yields better performance. It is good to have objects on the
    /// stack but performance starts getting worse as the size of the pool
    /// increases.
    std::vector<ObjectBlock> store_;

    std::size_t next_free_index_ = 0;
    bool exhausted_ = false;
};

}  // namespace Common
