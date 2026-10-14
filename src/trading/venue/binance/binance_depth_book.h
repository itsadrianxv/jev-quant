#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

#include "common/mem_pool.h"
#include "common/types.h"
#include "exchange/market_data/market_update.h"

namespace Trading {

/// Local Binance depth book rebuilt on pooled price-level nodes.
///
/// Each side keeps a sorted doubly linked list of price levels (bids
/// descending, asks ascending, so the list head is always the best price)
/// backed by a pooled-node MemPool and a price-slot hash array with linear
/// probing. This replaces the per-message node-churning std::map book: a
/// delta event now mutates existing nodes in place or reuses pooled blocks
/// instead of allocating tree nodes. The best bid/ask are the list heads, so
/// top-of-book reads are O(1).
///
/// The book also owns the pre-snapshot buffering: stream events that arrive
/// before the REST depth snapshot are held in a bounded ring and applied in
/// order right after the snapshot is installed, exactly like the previous
/// deque-based flow except the ring cannot grow without limit.
///
/// TODO: every event is still a JSON DOM allocation; a typed or SAX parse
/// writing straight into pooled storage is the follow-up (mempool spec).
class BinanceDepthBook final {
  public:
    /// Pooled price-level nodes per side. Sized for a 1000-level REST
    /// snapshot plus delta headroom; exhaustion fails fast.
    static constexpr std::size_t kMaxPriceLevelsPerSide = 4096;

    /// Price-slot hash array per side (power of two, at least twice the node
    /// capacity so the probe load factor stays below one half).
    static constexpr std::size_t kPriceSlotCount = 8192;

    /// Tombstones beyond this threshold trigger a slot-array rebuild.
    static constexpr std::size_t kMaxTombstonesPerSide = kPriceSlotCount / 4;

    /// Bounded ring for depth events buffered before the snapshot install.
    static constexpr std::size_t kPendingEventCapacity = 512;

    struct LevelView {
        double price = 0.0;
        double qty = 0.0;
    };

    BinanceDepthBook();

    BinanceDepthBook(const BinanceDepthBook&) = delete;
    BinanceDepthBook& operator=(const BinanceDepthBook&) = delete;

    /// Feed one stream depth event. Before the snapshot is installed the
    /// event is buffered in the pending ring; afterwards it is applied to
    /// the book. Throws std::runtime_error on a malformed event or a
    /// sequence gap, mirroring the previous ordered-map behavior.
    auto onDepthEvent(const nlohmann::json& message) -> void;

    /// Clear both sides, install the REST depth snapshot levels, drain the
    /// pending ring in order, and mark the book ready. Throws on a malformed
    /// snapshot or when a pending event reports a sequence gap.
    auto installSnapshot(const nlohmann::json& depth) -> void;

    /// Build the DEPTH_SNAPSHOT MarketUpdate published to the
    /// MarketDataConsumer: the best DEPTH_SNAPSHOT_LEVELS levels of each
    /// side walked from the list heads, identical to the previous
    /// ordered-map publishBook output.
    [[nodiscard]] auto buildMarketUpdate(Common::TickerId ticker_id) const
            -> Exchange::MarketUpdate;

    [[nodiscard]] auto snapshotReady() const noexcept -> bool {
        return snapshot_ready_;
    }

    [[nodiscard]] auto lastUpdateId() const noexcept -> std::uint64_t {
        return last_update_id_;
    }

    [[nodiscard]] auto pendingCount() const noexcept -> std::size_t {
        return pending_size_;
    }

    /// O(1) top of book, read from the sorted list heads.
    [[nodiscard]] auto bestBid() const noexcept -> std::optional<LevelView>;
    [[nodiscard]] auto bestAsk() const noexcept -> std::optional<LevelView>;

    [[nodiscard]] auto bidCount() const noexcept -> std::size_t {
        return bids_.level_count;
    }
    [[nodiscard]] auto askCount() const noexcept -> std::size_t {
        return asks_.level_count;
    }

  private:
    struct PriceLevelNode {
        double price = 0.0;
        double qty = 0.0;
        PriceLevelNode* prev = nullptr;
        PriceLevelNode* next = nullptr;
        std::uint32_t slot = 0;
    };

    /// One side of the book: pooled nodes, the sorted list, and the
    /// price-slot hash array.
    struct SideBook {
        explicit SideBook(bool bid) : is_bid(bid) {}

        bool is_bid = false;
        Common::MemPool<PriceLevelNode> pool{kMaxPriceLevelsPerSide};
        std::array<PriceLevelNode*, kPriceSlotCount> slots{};
        PriceLevelNode* head = nullptr;  // best price on this side
        PriceLevelNode* tail = nullptr;
        std::size_t level_count = 0;
        std::size_t tombstones = 0;
    };

    static auto priceBetter(const SideBook& side, double candidate,
                            double incumbent) noexcept -> bool {
        return side.is_bid ? candidate > incumbent : candidate < incumbent;
    }

    /// 64-bit mix of the raw double bits (a bijection), so distinct prices
    /// never collide before masking to the slot count.
    static auto priceHash(double price) noexcept -> std::uint64_t;
    static auto slotHome(double price) noexcept -> std::size_t;

    static auto findNode(const SideBook& side, double price) noexcept
            -> PriceLevelNode*;
    static auto insertSorted(SideBook& side, PriceLevelNode* node) noexcept
            -> void;
    static auto unlink(SideBook& side, PriceLevelNode* node) noexcept -> void;
    static auto insertSlot(SideBook& side, PriceLevelNode* node) noexcept
            -> void;
    static auto rehashSlots(SideBook& side) noexcept -> void;
    static auto clearSide(SideBook& side) noexcept -> void;

    static auto upsertPrice(SideBook& side, double price, double qty) -> void;
    static auto removePrice(SideBook& side, double price) -> void;

    auto applyDepthEvent(const nlohmann::json& message) -> void;
    auto drainPendingEvents() -> void;

    /// Sentinel marking a deleted slot: probing continues past it, and a
    /// later insert may reuse it. Never a valid node address.
    inline static PriceLevelNode* const kTombstone =
            reinterpret_cast<PriceLevelNode*>(std::uintptr_t{1});
    static constexpr std::size_t kNoSlot = kPriceSlotCount;

    SideBook bids_;
    SideBook asks_;
    std::uint64_t last_update_id_ = 0;
    bool snapshot_ready_ = false;
    std::array<nlohmann::json, kPendingEventCapacity> pending_events_{};
    std::size_t pending_head_ = 0;
    std::size_t pending_size_ = 0;
};

}  // namespace Trading
