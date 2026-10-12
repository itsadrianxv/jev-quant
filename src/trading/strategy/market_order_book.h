#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <sstream>
#include <string>

#include "common/macros.h"
#include "common/mem_pool.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"

namespace Trading {

/// Pooled incremental book capacities for one ticker book.
///
/// The order slot array and the order pool both hold 64K entries; each side's
/// level slot array and level pool hold 4096 entries. The open-addressed slot
/// arrays enforce a load-factor ceiling and fail fast before the pools could
/// ever exhaust, so probe chains stay short and a full slot array never
/// occurs.
///
/// With the eight TradeEngine ticker books this budgets, allocated cold at
/// startup:
///   - order pools:         8 * 64K * 32 B    = 16 MB   (BookOrder is 24 B)
///   - order slot arrays:   8 * 64K * 16 B    = 8 MB
///   - level pools:         8 * 2 * 4K * 40 B = 2.5 MB  (PriceLevel is 32 B)
///   - level slot arrays:   8 * 2 * 4K * 16 B = 1 MB
/// for roughly 27.5 MB in total.
inline constexpr std::size_t kMaxBookOrderSlots = 64 * 1024;
inline constexpr std::size_t kMaxBookLevelSlotsPerSide = 4 * 1024;

/// Load-factor ceiling for the open-addressed slot arrays: insertions beyond
/// this fraction of the slots fail fast, keeping probe chains short and
/// guaranteeing that an empty slot always exists.
inline constexpr std::size_t kMaxSlotLoadFactorNumerator = 3;
inline constexpr std::size_t kMaxSlotLoadFactorDenominator = 4;

inline constexpr std::size_t kMaxLiveOrderEntries =
        kMaxBookOrderSlots * kMaxSlotLoadFactorNumerator /
        kMaxSlotLoadFactorDenominator;
inline constexpr std::size_t kMaxLiveLevelEntriesPerSide =
        kMaxBookLevelSlotsPerSide * kMaxSlotLoadFactorNumerator /
        kMaxSlotLoadFactorDenominator;

struct BBO {
    Common::Price bid_price_ = Common::Price_INVALID;
    Common::Price ask_price_ = Common::Price_INVALID;
    Common::Qty bid_qty_ = Common::Qty_INVALID;
    Common::Qty ask_qty_ = Common::Qty_INVALID;

    auto toString() const -> std::string {
        std::stringstream stream;
        stream << "BBO[bid:" << bid_qty_ << "@" << bid_price_
                      << " ask:" << ask_price_ << "@" << ask_qty_ << "]";
        return stream.str();
    }
};

/// A participant-side local book that accepts both reference increments and
/// venue-provided depth snapshots.
///
/// The incremental book follows the reference book's own pooled order-book
/// idiom (Building Low-Latency Applications with C++, chapter 12): pooled
/// BookOrder and PriceLevel nodes held in Common::MemPool storage, per-side
/// sorted circular doubly linked price level lists, and BBO derived from the
/// sorted list head pointers in O(1).
///
/// Documented divergence from the reference book: the reference indexes order
/// and price slots with direct modulo arithmetic and assumes collision-free
/// slots because its order identifiers are small and contiguous.
/// Venue-assigned order identifiers here are large non-contiguous 64-bit
/// values, so collision-free modulo slots cannot be assumed; the slot arrays
/// use open addressing with linear probing and a load-factor ceiling instead.
/// Slot exhaustion fails fast by throwing before the pools could exhaust, and
/// pool blocks are marked free without clearing their memory on CLEAR and on
/// snapshot install.
///
/// TODO(mempool): the pooled-book idiom here intentionally duplicates ticket
/// 02's Binance depth book for now (MVP); extract a shared pooled-book
/// component once both call sites have settled.
class MarketOrderBook final {
  public:
    enum class BookMode : std::uint8_t {
        UNINITIALIZED = 0,
        INCREMENTAL = 1,
        DEPTH_SNAPSHOT = 2,
    };

    explicit MarketOrderBook(Common::TickerId ticker_id)
            : ticker_id_(ticker_id),
              order_pool_(kMaxBookOrderSlots),
              bid_level_pool_(kMaxBookLevelSlotsPerSide),
              ask_level_pool_(kMaxBookLevelSlotsPerSide) {}

    MarketOrderBook(const MarketOrderBook&) = delete;
    MarketOrderBook& operator=(const MarketOrderBook&) = delete;
    MarketOrderBook(MarketOrderBook&&) = delete;
    MarketOrderBook& operator=(MarketOrderBook&&) = delete;

    auto onMarketUpdate(const Exchange::MarketUpdate& update) -> void {
        switch (update.type_) {
            case Exchange::MarketUpdateType::ADD:
                requireIncrementalMode();
                addOrder(update);
                break;
            case Exchange::MarketUpdateType::MODIFY:
                requireIncrementalMode();
                modifyOrder(update);
                break;
            case Exchange::MarketUpdateType::CANCEL:
                requireIncrementalMode();
                cancelOrder(update.order_id_);
                break;
            case Exchange::MarketUpdateType::CLEAR:
                requireIncrementalMode();
                clearIncrementalBook();
                break;
            case Exchange::MarketUpdateType::DEPTH_SNAPSHOT:
                requireDepthSnapshotMode();
                onDepthSnapshot(update.depth_snapshot_);
                return;
            case Exchange::MarketUpdateType::TRADE:
                requireIncrementalMode();
                return;
            case Exchange::MarketUpdateType::INVALID:
            case Exchange::MarketUpdateType::SNAPSHOT_START:
            case Exchange::MarketUpdateType::SNAPSHOT_END:
                return;
        }

        updateBBOFromIncrementalBook();
    }

    auto onDepthSnapshot(const Exchange::DepthSnapshot& snapshot) -> void {
        if (mode_ == BookMode::INCREMENTAL) {
            throw std::logic_error(
                    "MarketOrderBook cannot mix incremental and depth snapshot updates");
        }
        clearIncrementalBook();
        depth_snapshot_ = snapshot;
        mode_ = BookMode::DEPTH_SNAPSHOT;
        updateBBOFromDepthSnapshot();
    }

    [[nodiscard]] auto getBBO() const noexcept -> const BBO* {
        return &bbo_;
    }

    [[nodiscard]] auto getDepthSnapshot() const noexcept
            -> const Exchange::DepthSnapshot* {
        return &depth_snapshot_;
    }

    [[nodiscard]] auto tickerId() const noexcept -> Common::TickerId {
        return ticker_id_;
    }

    [[nodiscard]] auto mode() const noexcept -> BookMode {
        return mode_;
    }

    [[nodiscard]] auto toString() const -> std::string {
        std::stringstream stream;
        stream << "MarketOrderBook[ticker:" << ticker_id_
                      << " mode:" << modeToString(mode_)
                      << " " << bbo_.toString() << "]";
        return stream.str();
    }

  private:
    /// A single resting order tracked by the incremental book. Pooled node;
    /// the destructor is never called, so the struct stays trivially
    /// destructible and every field carries its own reset default.
    struct BookOrder {
        Common::OrderId order_id_ = Common::OrderId_INVALID;
        Common::Price price_ = Common::Price_INVALID;
        Common::Qty qty_ = Common::Qty_INVALID;
        Common::Side side_ = Common::Side::INVALID;

        BookOrder() = default;

        BookOrder(Common::OrderId order_id, Common::Price price, Common::Qty qty,
                  Common::Side side)
                : order_id_(order_id), price_(price), qty_(qty), side_(side) {}
    };

    /// One aggregated price level. Pooled node that also serves as an entry
    /// in its side's sorted circular doubly linked level list, ordered from
    /// the best price at the head to the worst price before the head.
    struct PriceLevel {
        Common::Price price_ = Common::Price_INVALID;
        Common::Qty qty_ = 0;
        Common::Side side_ = Common::Side::INVALID;
        PriceLevel* prev_level_ = nullptr;
        PriceLevel* next_level_ = nullptr;

        PriceLevel() = default;

        PriceLevel(Common::Price price, Common::Qty qty, Common::Side side,
                   PriceLevel* prev_level, PriceLevel* next_level)
                : price_(price), qty_(qty), side_(side), prev_level_(prev_level),
                  next_level_(next_level) {}
    };

    static_assert(sizeof(BookOrder) == 24, "Order pool budget assumes a 24-byte BookOrder");
    static_assert(sizeof(PriceLevel) == 32,
                  "Level pool budget assumes a 32-byte PriceLevel");

    /// Open-addressed slot array mapping keys to pooled node pointers with
    /// linear probing. Keys are hashed by masking (the slot count is a power
    /// of two); callers enforce the load-factor ceiling before insert, so a
    /// free slot always exists and probe loops terminate. Deletion performs
    /// backward-shift compaction so no probe chain is ever broken and no
    /// tombstones accumulate. insert() requires the key to be absent.
    template <typename KeyT, typename NodeT, std::size_t kSlots>
    class SlotArray final {
        static_assert((kSlots & (kSlots - 1)) == 0, "Slot count must be a power of two");

      public:
        struct Entry {
            KeyT key_ = {};
            NodeT* node_ = nullptr;
        };

        [[nodiscard]] auto find(KeyT key) const noexcept -> NodeT* {
            auto index = indexOf(key);
            while (entries_[index].node_ != nullptr) {
                if (entries_[index].key_ == key) {
                    return entries_[index].node_;
                }
                index = (index + 1) & kMask;
            }
            return nullptr;
        }

        auto insert(KeyT key, NodeT* node) noexcept -> void {
            auto index = indexOf(key);
            while (entries_[index].node_ != nullptr) {
                index = (index + 1) & kMask;
            }
            entries_[index].key_ = key;
            entries_[index].node_ = node;
            ++live_count_;
        }

        /// Remove the entry for key and return its node, or nullptr when the
        /// key is absent. The removal hole is compacted by shifting forward
        /// entries whose probe chain passes through it.
        auto erase(KeyT key) noexcept -> NodeT* {
            auto index = indexOf(key);
            while (entries_[index].node_ != nullptr && entries_[index].key_ != key) {
                index = (index + 1) & kMask;
            }
            if (entries_[index].node_ == nullptr) {
                return nullptr;
            }

            NodeT* removed = entries_[index].node_;
            entries_[index] = Entry{};
            auto hole = index;
            auto probe = index;
            while (true) {
                probe = (probe + 1) & kMask;
                if (entries_[probe].node_ == nullptr) {
                    break;
                }
                const auto home = indexOf(entries_[probe].key_);
                if (((home - hole) & kMask) < ((probe - hole) & kMask)) {
                    entries_[hole] = entries_[probe];
                    entries_[probe] = Entry{};
                    hole = probe;
                }
            }
            --live_count_;
            return removed;
        }

        /// Mark every block free without clearing the pooled nodes' memory:
        /// the slot array itself is reset, the pool keeps its storage warm.
        template <typename Visitor>
        auto forEachNode(Visitor&& visitor) noexcept -> void {
            for (auto& entry : entries_) {
                if (entry.node_ != nullptr) {
                    visitor(entry.node_);
                }
            }
        }

        auto clear() noexcept -> void {
            entries_.fill(Entry{});
            live_count_ = 0;
        }

        [[nodiscard]] auto liveCount() const noexcept -> std::size_t {
            return live_count_;
        }

      private:
        static constexpr std::size_t kMask = kSlots - 1;

        static auto indexOf(KeyT key) noexcept -> std::size_t {
            return static_cast<std::size_t>(key) & kMask;
        }

        std::array<Entry, kSlots> entries_{};
        std::size_t live_count_ = 0;
    };

    using OrderSlots = SlotArray<Common::OrderId, BookOrder, kMaxBookOrderSlots>;
    using LevelSlots = SlotArray<Common::Price, PriceLevel, kMaxBookLevelSlotsPerSide>;

    [[nodiscard]] auto levelSlots(Common::Side side) noexcept -> LevelSlots& {
        return side == Common::Side::BUY ? bid_level_slots_ : ask_level_slots_;
    }

    [[nodiscard]] auto levelPool(Common::Side side) noexcept
            -> Common::MemPool<PriceLevel>& {
        return side == Common::Side::BUY ? bid_level_pool_ : ask_level_pool_;
    }

    [[nodiscard]] auto levelHead(Common::Side side) noexcept -> PriceLevel*& {
        return side == Common::Side::BUY ? bids_head_ : asks_head_;
    }

    /// Fail fast when a new price level would be needed on this side and the
    /// side is already at its load-factor ceiling. Existing levels are always
    /// allowed through.
    auto requireLevelCapacity(Common::Side side, Common::Price price) -> void {
        if (levelSlots(side).find(price) != nullptr) {
            return;
        }
        if (UNLIKELY(levelSlots(side).liveCount() >= kMaxLiveLevelEntriesPerSide)) {
            throw std::runtime_error("MarketOrderBook price level slot capacity exhausted");
        }
    }

    auto addOrder(const Exchange::MarketUpdate& update) -> void {
        cancelOrder(update.order_id_);
        if (UNLIKELY(order_slots_.liveCount() >= kMaxLiveOrderEntries)) {
            throw std::runtime_error("MarketOrderBook order slot capacity exhausted");
        }
        requireLevelCapacity(update.side_, update.price_);
        BookOrder* order = order_pool_.allocate(update.order_id_, update.price_,
                                                update.qty_, update.side_);
        order_slots_.insert(update.order_id_, order);
        addToLevel(update.side_, update.price_, update.qty_);
    }

    auto modifyOrder(const Exchange::MarketUpdate& update) -> void {
        BookOrder* order = order_slots_.find(update.order_id_);
        if (order == nullptr) {
            return;
        }

        if (order->side_ != update.side_ || order->price_ != update.price_) {
            requireLevelCapacity(update.side_, update.price_);
            removeFromLevel(order->side_, order->price_, order->qty_);
            order->side_ = update.side_;
            order->price_ = update.price_;
            addToLevel(order->side_, order->price_, update.qty_);
        } else {
            if (update.qty_ >= order->qty_) {
                addToLevel(order->side_, order->price_, update.qty_ - order->qty_);
            } else {
                removeFromLevel(order->side_, order->price_, order->qty_ - update.qty_);
            }
        }
        order->qty_ = update.qty_;
    }

    auto cancelOrder(Common::OrderId order_id) -> void {
        BookOrder* order = order_slots_.erase(order_id);
        if (order == nullptr) {
            return;
        }

        removeFromLevel(order->side_, order->price_, order->qty_);
        order_pool_.deallocate(order);
    }

    auto addToLevel(Common::Side side, Common::Price price, Common::Qty qty)
            -> void {
        PriceLevel* level = levelSlots(side).find(price);
        if (level == nullptr) {
            level = createLevel(side, price);
        }
        level->qty_ += qty;
    }

    auto removeFromLevel(Common::Side side, Common::Price price, Common::Qty qty)
            -> void {
        PriceLevel* level = levelSlots(side).find(price);
        if (level == nullptr) {
            return;
        }
        if (level->qty_ <= qty) {
            removeLevelFromSortedList(side, level);
            levelSlots(side).erase(price);
            levelPool(side).deallocate(level);
        } else {
            level->qty_ -= qty;
        }
    }

    auto createLevel(Common::Side side, Common::Price price) -> PriceLevel* {
        auto& slots = levelSlots(side);
        if (UNLIKELY(slots.liveCount() >= kMaxLiveLevelEntriesPerSide)) {
            throw std::runtime_error("MarketOrderBook price level slot capacity exhausted");
        }
        PriceLevel* level = levelPool(side).allocate(price, /*qty=*/0, side,
                                                     nullptr, nullptr);
        slots.insert(price, level);
        insertLevelIntoSortedList(side, level);
        return level;
    }

    /// A price is better when it is more aggressive for the side: higher for
    /// bids, lower for asks.
    static auto isBetterLevelPrice(Common::Side side, Common::Price left,
                                   Common::Price right) noexcept -> bool {
        return side == Common::Side::BUY ? left > right : left < right;
    }

    /// Link the level into its side's sorted circular doubly linked list,
    /// keeping the best price at the head pointer.
    auto insertLevelIntoSortedList(Common::Side side, PriceLevel* level) noexcept
            -> void {
        PriceLevel*& head = levelHead(side);
        if (head == nullptr) {
            level->prev_level_ = level;
            level->next_level_ = level;
            head = level;
            return;
        }

        // Walk from the best level towards the worst until the first level
        // that is not strictly better than the new one; the new level is
        // inserted directly before it.
        PriceLevel* target = head;
        while (isBetterLevelPrice(side, target->price_, level->price_)) {
            target = target->next_level_;
            if (target == head) {
                break;  // every resting level is better: insert at the tail
            }
        }

        level->prev_level_ = target->prev_level_;
        level->next_level_ = target;
        target->prev_level_->next_level_ = level;
        target->prev_level_ = level;

        if (isBetterLevelPrice(side, level->price_, head->price_)) {
            head = level;
        }
    }

    auto removeLevelFromSortedList(Common::Side side, PriceLevel* level) noexcept
            -> void {
        PriceLevel*& head = levelHead(side);
        if (level->next_level_ == level) {  // only level on this side
            head = nullptr;
        } else {
            level->prev_level_->next_level_ = level->next_level_;
            level->next_level_->prev_level_ = level->prev_level_;
            if (head == level) {
                head = level->next_level_;
            }
        }
        level->prev_level_ = level->next_level_ = nullptr;
    }

    auto clearSideLevels(Common::Side side) noexcept -> void {
        PriceLevel* head = levelHead(side);
        if (head != nullptr) {
            PriceLevel* level = head->next_level_;
            while (level != head) {
                PriceLevel* next = level->next_level_;
                levelPool(side).deallocate(level);
                level = next;
            }
            levelPool(side).deallocate(head);
        }
        levelHead(side) = nullptr;
        levelSlots(side).clear();
    }

    /// Reset the incremental book: every order and level block is marked free
    /// in its pool without clearing the block memory, so subsequent
    /// allocations reuse the same storage.
    auto clearIncrementalBook() noexcept -> void {
        order_slots_.forEachNode(
                [this](BookOrder* order) { order_pool_.deallocate(order); });
        order_slots_.clear();
        clearSideLevels(Common::Side::BUY);
        clearSideLevels(Common::Side::SELL);
        bbo_ = BBO{};
    }

    /// BBO is read straight off the sorted level list head pointers in O(1).
    auto updateBBOFromIncrementalBook() noexcept -> void {
        if (bids_head_ != nullptr) {
            bbo_.bid_price_ = bids_head_->price_;
            bbo_.bid_qty_ = bids_head_->qty_;
        } else {
            bbo_.bid_price_ = Common::Price_INVALID;
            bbo_.bid_qty_ = Common::Qty_INVALID;
        }
        if (asks_head_ != nullptr) {
            bbo_.ask_price_ = asks_head_->price_;
            bbo_.ask_qty_ = asks_head_->qty_;
        } else {
            bbo_.ask_price_ = Common::Price_INVALID;
            bbo_.ask_qty_ = Common::Qty_INVALID;
        }
    }

    auto updateBBOFromDepthSnapshot() -> void {
        const auto& bid = depth_snapshot_.bids_.front();
        const auto& ask = depth_snapshot_.asks_.front();
        bbo_.bid_price_ = bid.price_;
        bbo_.bid_qty_ = bid.qty_;
        bbo_.ask_price_ = ask.price_;
        bbo_.ask_qty_ = ask.qty_;
    }

    static auto modeToString(BookMode mode) -> const char* {
        switch (mode) {
            case BookMode::UNINITIALIZED:
                return "UNINITIALIZED";
            case BookMode::INCREMENTAL:
                return "INCREMENTAL";
            case BookMode::DEPTH_SNAPSHOT:
                return "DEPTH_SNAPSHOT";
        }
        return "UNKNOWN";
    }

    auto requireIncrementalMode() -> void {
        if (mode_ == BookMode::DEPTH_SNAPSHOT) {
            throw std::logic_error(
                    "MarketOrderBook cannot mix depth snapshot and incremental updates");
        }
        mode_ = BookMode::INCREMENTAL;
    }

    auto requireDepthSnapshotMode() -> void {
        if (mode_ == BookMode::INCREMENTAL) {
            throw std::logic_error(
                    "MarketOrderBook cannot mix incremental and depth snapshot updates");
        }
        mode_ = BookMode::DEPTH_SNAPSHOT;
    }

    const Common::TickerId ticker_id_;
    BookMode mode_ = BookMode::UNINITIALIZED;
    Exchange::DepthSnapshot depth_snapshot_{};
    BBO bbo_{};

    Common::MemPool<BookOrder> order_pool_;
    Common::MemPool<PriceLevel> bid_level_pool_;
    Common::MemPool<PriceLevel> ask_level_pool_;

    OrderSlots order_slots_;
    LevelSlots bid_level_slots_;
    LevelSlots ask_level_slots_;

    /// Best (head) level per side; null when the side is empty.
    PriceLevel* bids_head_ = nullptr;
    PriceLevel* asks_head_ = nullptr;
};

using MarketOrderBookHashMap =
        std::array<MarketOrderBook*, Common::ME_MAX_TICKERS>;

}  // namespace Trading