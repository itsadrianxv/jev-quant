#include "trading/venue/binance_depth_book.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace Trading {
namespace {
// The previous implementation read the venue price strings with
// std::stod(level[0].get<std::string>()), which copies the level string out
// of the DOM first. Reading the stored string reference and converting it
// directly removes the per-level temporary string.
auto levelPrice(const nlohmann::json& level) -> double {
    return std::stod(level[0].get_ref<const std::string&>());
}
auto levelQuantity(const nlohmann::json& level) -> double {
    return std::stod(level[1].get_ref<const std::string&>());
}
}  // namespace

BinanceDepthBook::BinanceDepthBook() : bids_(true), asks_(false) {}

auto BinanceDepthBook::priceHash(double price) noexcept -> std::uint64_t {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &price, sizeof(bits));
    // splitmix64 finalizer: a bijection on the bit pattern.
    bits += 0x9e3779b97f4a7c15;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111eb;
    return bits ^ (bits >> 31);
}

auto BinanceDepthBook::slotHome(double price) noexcept -> std::size_t {
    return static_cast<std::size_t>(priceHash(price)) & (kPriceSlotCount - 1);
}

auto BinanceDepthBook::findNode(const SideBook& side, double price) noexcept
        -> PriceLevelNode* {
    auto index = slotHome(price);
    while (true) {
        const auto entry = side.slots[index];
        if (entry == nullptr) return nullptr;
        if (entry != kTombstone && entry->price == price) return entry;
        index = (index + 1) & (kPriceSlotCount - 1);
    }
}

auto BinanceDepthBook::insertSorted(SideBook& side,
                                    PriceLevelNode* node) noexcept -> void {
    // Walk from the best-price head to the first level the new price beats.
    PriceLevelNode* previous = nullptr;
    auto* current = side.head;
    while (current != nullptr && !priceBetter(side, node->price, current->price)) {
        previous = current;
        current = current->next;
    }
    node->prev = previous;
    node->next = current;
    if (previous != nullptr) previous->next = node; else side.head = node;
    if (current != nullptr) current->prev = node; else side.tail = node;
    ++side.level_count;
}

auto BinanceDepthBook::unlink(SideBook& side, PriceLevelNode* node) noexcept
        -> void {
    if (node->prev != nullptr) node->prev->next = node->next;
    else side.head = node->next;
    if (node->next != nullptr) node->next->prev = node->prev;
    else side.tail = node->prev;
}

auto BinanceDepthBook::insertSlot(SideBook& side, PriceLevelNode* node) noexcept
        -> void {
    // Linear probing: the first free slot wins, a tombstone is reusable but
    // probing must continue to rule out the key living further along the
    // chain.
    auto index = slotHome(node->price);
    auto reusable = kNoSlot;
    while (true) {
        const auto entry = side.slots[index];
        if (entry == nullptr) break;
        if (entry == kTombstone && reusable == kNoSlot) reusable = index;
        index = (index + 1) & (kPriceSlotCount - 1);
    }
    const auto target = reusable != kNoSlot ? reusable : index;
    side.slots[target] = node;
    node->slot = static_cast<std::uint32_t>(target);
}

auto BinanceDepthBook::rehashSlots(SideBook& side) noexcept -> void {
    side.slots.fill(nullptr);
    side.tombstones = 0;
    for (auto* node = side.head; node != nullptr; node = node->next) {
        auto index = slotHome(node->price);
        while (side.slots[index] != nullptr) {
            index = (index + 1) & (kPriceSlotCount - 1);
        }
        side.slots[index] = node;
        node->slot = static_cast<std::uint32_t>(index);
    }
}

auto BinanceDepthBook::clearSide(SideBook& side) noexcept -> void {
    auto* node = side.head;
    while (node != nullptr) {
        auto* next = node->next;
        side.pool.deallocate(node);
        node = next;
    }
    side.slots.fill(nullptr);
    side.head = nullptr;
    side.tail = nullptr;
    side.level_count = 0;
    side.tombstones = 0;
}

auto BinanceDepthBook::upsertPrice(SideBook& side, double price,
                                   double qty) -> void {
    if (auto* node = findNode(side, price)) {
        node->qty = qty;
        return;
    }
    auto* node = side.pool.allocate();
    node->price = price;
    node->qty = qty;
    insertSorted(side, node);
    insertSlot(side, node);
}

auto BinanceDepthBook::removePrice(SideBook& side, double price) -> void {
    auto* node = findNode(side, price);
    if (node == nullptr) return;
    side.slots[node->slot] = kTombstone;
    ++side.tombstones;
    unlink(side, node);
    side.pool.deallocate(node);
    --side.level_count;
    if (side.tombstones > kMaxTombstonesPerSide) rehashSlots(side);
}

auto BinanceDepthBook::onDepthEvent(const nlohmann::json& message) -> void {
    if (!snapshot_ready_) {
        if (pending_size_ == kPendingEventCapacity) {
            FATAL("Binance pending depth events ring overflow");
        }
        pending_events_[(pending_head_ + pending_size_) %
                        kPendingEventCapacity] = message;
        ++pending_size_;
        return;
    }
    applyDepthEvent(message);
}

auto BinanceDepthBook::applyDepthEvent(const nlohmann::json& message) -> void {
    if (!message.contains("U") || !message.contains("u") ||
        !message.contains("pu") || !message.contains("b") ||
        !message.contains("a")) {
        throw std::runtime_error("Invalid Binance depth event");
    }
    const auto first = message.at("U").get<std::uint64_t>();
    const auto final = message.at("u").get<std::uint64_t>();
    const auto previous = message.at("pu").get<std::uint64_t>();
    if (final <= last_update_id_) return;
    if (last_update_id_ != 0 && previous != last_update_id_ &&
        !(first <= last_update_id_ + 1 && final >= last_update_id_ + 1)) {
        throw std::runtime_error(
                "Binance depth sequence gap: U=" + std::to_string(first) +
                " u=" + std::to_string(final) +
                " pu=" + std::to_string(previous) +
                " last=" + std::to_string(last_update_id_));
    }
    for (const auto& level : message.at("b")) {
        const auto price = levelPrice(level);
        const auto quantity = levelQuantity(level);
        if (quantity == 0.0) removePrice(bids_, price);
        else upsertPrice(bids_, price, quantity);
    }
    for (const auto& level : message.at("a")) {
        const auto price = levelPrice(level);
        const auto quantity = levelQuantity(level);
        if (quantity == 0.0) removePrice(asks_, price);
        else upsertPrice(asks_, price, quantity);
    }
    last_update_id_ = final;
}

auto BinanceDepthBook::installSnapshot(const nlohmann::json& depth) -> void {
    clearSide(bids_);
    clearSide(asks_);
    for (const auto& level : depth.at("bids")) {
        upsertPrice(bids_, levelPrice(level), levelQuantity(level));
    }
    for (const auto& level : depth.at("asks")) {
        upsertPrice(asks_, levelPrice(level), levelQuantity(level));
    }
    last_update_id_ = depth.at("lastUpdateId").get<std::uint64_t>();
    drainPendingEvents();
    snapshot_ready_ = true;
}

auto BinanceDepthBook::drainPendingEvents() -> void {
    while (pending_size_ > 0) {
        auto& event = pending_events_[pending_head_];
        applyDepthEvent(event);
        event = nlohmann::json{};
        pending_head_ = (pending_head_ + 1) % kPendingEventCapacity;
        --pending_size_;
    }
}

auto BinanceDepthBook::buildMarketUpdate(Common::TickerId ticker_id) const
        -> Exchange::MarketUpdate {
    Exchange::MarketUpdate update;
    update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update.ticker_id_ = ticker_id;
    std::size_t index = 0;
    for (auto* node = bids_.head;
         node != nullptr && index < Common::DEPTH_SNAPSHOT_LEVELS;
         node = node->next, ++index) {
        update.depth_snapshot_.bids_[index].price_ =
                static_cast<Common::Price>(node->price * 100000000.0);
        update.depth_snapshot_.bids_[index].qty_ =
                static_cast<Common::Qty>(node->qty);
    }
    index = 0;
    for (auto* node = asks_.head;
         node != nullptr && index < Common::DEPTH_SNAPSHOT_LEVELS;
         node = node->next, ++index) {
        update.depth_snapshot_.asks_[index].price_ =
                static_cast<Common::Price>(node->price * 100000000.0);
        update.depth_snapshot_.asks_[index].qty_ =
                static_cast<Common::Qty>(node->qty);
    }
    return update;
}

auto BinanceDepthBook::bestBid() const noexcept -> std::optional<LevelView> {
    if (bids_.head == nullptr) return std::nullopt;
    return LevelView{bids_.head->price, bids_.head->qty};
}

auto BinanceDepthBook::bestAsk() const noexcept -> std::optional<LevelView> {
    if (asks_.head == nullptr) return std::nullopt;
    return LevelView{asks_.head->price, asks_.head->qty};
}

}  // namespace Trading
