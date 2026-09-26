#include "trading/venue/simex_venue_adapter.h"
#include "transport/protocol.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace Trading {
namespace {
namespace Wire = simex::transport;
namespace Venue = simex::common;
using Clock = std::chrono::steady_clock;

auto address(std::uint16_t port) -> sockaddr_in {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    result.sin_port = htons(port);
    return result;
}

auto wouldBlock() -> bool { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

auto side(Venue::Side value) -> Common::Side {
    if (value == Venue::Side::BUY) return Common::Side::BUY;
    if (value == Venue::Side::SELL) return Common::Side::SELL;
    throw std::runtime_error("Invalid simex side");
}

auto effect(Common::OrderOffset offset) -> Venue::PositionEffect {
    switch (offset) {
        case Common::OrderOffset::OPEN: return Venue::PositionEffect::OPEN;
        case Common::OrderOffset::CLOSE_TODAY: return Venue::PositionEffect::CLOSE_TODAY;
        case Common::OrderOffset::CLOSE_YESTERDAY: return Venue::PositionEffect::CLOSE_YESTERDAY;
        default: throw std::invalid_argument("Simex requires an explicit position effect");
    }
}

auto validPrice(Common::Price price) -> bool { return price > 0 && price != Common::Price_INVALID; }
auto validQty(Common::Qty qty) -> bool { return qty > 0 && qty != Common::Qty_INVALID; }

void checkHeader(const Wire::FrameHeader& header, Wire::MessageKind kind, std::size_t payload) {
    if (header.version != Wire::kProtocolVersion || header.reserved != 0 ||
        header.kind != static_cast<std::uint8_t>(kind) || header.payload_size != payload)
        throw std::runtime_error("Simex frame version, kind, or size mismatch");
}
}

auto SimexVenueConfig::fromJson(const nlohmann::json& value, Common::TickerId ticker) -> SimexVenueConfig {
    SimexVenueConfig config;
    config.host = value.value("host", std::string("127.0.0.1"));
    const auto tcp = value.value("tcp_port", 19001);
    const auto udp = value.value("udp_port", 19002);
    const auto connect_ms = value.value("connect_timeout_ms", 5000);
    const auto feed_ms = value.value("feed_timeout_ms", 5000);
    const auto capacity = value.value("buffer_capacity", 4096);
    if (config.host != "127.0.0.1" || tcp < 1 || tcp > 65535 || udp < 1 || udp > 65535 ||
        connect_ms < 1 || feed_ms < 1 || capacity < 1 || capacity > 1048576 || ticker >= Common::ME_MAX_TICKERS)
        throw std::invalid_argument("Invalid simex venue configuration");
    config.tcp_port = static_cast<std::uint16_t>(tcp);
    config.udp_port = static_cast<std::uint16_t>(udp);
    config.ticker_id = ticker;
    config.connect_timeout = std::chrono::milliseconds(connect_ms);
    config.feed_timeout = std::chrono::milliseconds(feed_ms);
    config.capacity = static_cast<std::size_t>(capacity);
    return config;
}

struct SimexVenueAdapter::Impl {
    struct Order {
        Exchange::ClientRequest request;
        Common::Qty remaining = 0;
        bool terminal = false;
        bool cancel_pending = false;
    };
    MarketDataConsumer* market;
    OrderGateway* gateway;
    SimexVenueConfig config;
    std::atomic<bool> running{false};
    bool started = false;
    int tcp = -1, udp = -1;
    bool market_started = false, synchronized = false, usable = false;
    std::uint64_t request_sequence = 0, response_sequence = 0, market_sequence = 0, snapshot_sequence = 0;
    std::size_t participant_response_sequence = 1;
    std::deque<Wire::WireRequest> outgoing;
    std::size_t sent = 0, received = 0;
    Wire::WireResponse incoming{};
    Clock::time_point last_packet{}, last_write{}, ahead_since{};
    std::uint64_t advertised_sequence = 0;
    std::unordered_map<Common::OrderId, Order> orders;
    std::unordered_map<Venue::MarketOrderId, simex::exchange::MarketUpdate> book;
    std::map<std::uint64_t, simex::exchange::MarketUpdate> pending;
    Common::Price last_trade = Common::Price_INVALID;
    std::int64_t net_position = 0;

    Impl(MarketDataConsumer* md, OrderGateway* og, SimexVenueConfig cfg)
        : market(md), gateway(og), config(std::move(cfg)) {
        if (!market || !gateway || market->clientId() != gateway->clientId() ||
            gateway->clientId() == Common::ClientId_INVALID)
            throw std::invalid_argument("Invalid simex participant boundaries");
    }
    ~Impl() { stop(); }

    void start() {
        if (started) throw std::logic_error("Simex adapter requires a fresh process for each run");
        started = true;
        try {
            udp = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            tcp = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (udp < 0 || tcp < 0) throw std::runtime_error("Cannot create simex sockets");
            const auto local = address(config.udp_port);
            if (::bind(udp, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0)
                throw std::runtime_error("Cannot bind simex UDP receive port");
            const auto remote = address(config.tcp_port);
            if (::connect(tcp, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) < 0) {
                if (errno != EINPROGRESS) throw std::runtime_error("Cannot connect to simex TCP gateway");
                pollfd descriptor{tcp, POLLOUT, 0};
                if (::poll(&descriptor, 1, static_cast<int>(config.connect_timeout.count())) <= 0)
                    throw std::runtime_error("Simex TCP connection timeout");
                int error = 0;
                socklen_t length = sizeof(error);
                if (::getsockopt(tcp, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0)
                    throw std::runtime_error("Simex TCP connection failed");
            }
            last_packet = Clock::now();
            gateway->setRequestHandler([this](std::size_t, const auto& request) { submit(request); });
            gateway->setPollHandler([this] { poll(); });
            running.store(true);
            gateway->start();
            std::clog << "event=simex_connected state=waiting_for_market_data\n";
        } catch (...) { stop(); throw; }
    }

    void stop() {
        running.store(false);
        gateway->stop();
        gateway->setRequestHandler({});
        gateway->setPollHandler({});
        market->stop();
        if (tcp >= 0) { ::close(tcp); tcp = -1; }
        if (udp >= 0) { ::close(udp); udp = -1; }
    }

    void reject(const Exchange::ClientRequest& request, const char* reason) {
        Exchange::ClientResponse response;
        response.type_ = request.type_ == Exchange::ClientRequestType::CANCEL
                ? Exchange::ClientResponseType::CANCEL_REJECTED : Exchange::ClientResponseType::REJECTED;
        response.client_id_ = gateway->clientId();
        response.ticker_id_ = request.ticker_id_;
        response.client_order_id_ = request.order_id_;
        response.side_ = request.side_;
        response.price_ = request.price_;
        response.exec_qty_ = 0;
        response.leaves_qty_ = request.qty_;
        response.order_type_ = request.order_type_;
        response.offset_ = request.offset_;
        gateway->publishClientResponse(participant_response_sequence++, response);
        std::clog << "event=simex_local_rejection order_id=" << request.order_id_ << " reason=" << reason << '\n';
    }

    void submit(const Exchange::ClientRequest& request) {
        const auto is_cancel = request.type_ == Exchange::ClientRequestType::CANCEL;
        if (request.client_id_ != gateway->clientId() || request.ticker_id_ != config.ticker_id ||
            request.order_id_ == Common::OrderId_INVALID ||
            (request.side_ != Common::Side::BUY && request.side_ != Common::Side::SELL) ||
            (!is_cancel && request.type_ != Exchange::ClientRequestType::NEW)) {
            reject(request, "invalid_request"); return;
        }
        if (!is_cancel && (!usable || !validQty(request.qty_) || request.qty_ > static_cast<Common::Qty>(INT32_MAX) ||
            (request.order_type_ != Common::OrderType::LIMIT && request.order_type_ != Common::OrderType::MARKET) ||
            (request.order_type_ == Common::OrderType::LIMIT && !validPrice(request.price_)))) {
            reject(request, "unsupported_order_or_unusable_depth"); return;
        }
        auto found = orders.find(request.order_id_);
        if (is_cancel && (found == orders.end() || found->second.terminal || found->second.cancel_pending)) {
            reject(request, "order_not_cancelable"); return;
        }
        if (!is_cancel && found != orders.end()) { reject(request, "duplicate_order_id"); return; }
        if (!is_cancel && request.offset_ == Common::OrderOffset::OPEN) {
            const bool buy = request.side_ == Common::Side::BUY;
            bool opposite = (buy && net_position < 0) || (!buy && net_position > 0);
            for (const auto& [id, order] : orders) {
                (void)id;
                opposite = opposite || (!order.terminal && order.request.offset_ == Common::OrderOffset::OPEN &&
                                         order.request.side_ != request.side_);
            }
            if (opposite) { reject(request, "opposite_open_exposure"); return; }
        }
        const auto& original = is_cancel ? found->second.request : request;
        Wire::WireRequest wire{};
        try { wire.request.position_effect = effect(original.offset_); }
        catch (const std::invalid_argument&) { reject(request, "unsupported_offset"); return; }
        wire.header.kind = static_cast<std::uint8_t>(Wire::MessageKind::REQUEST);
        wire.header.payload_size = sizeof(wire.request);
        wire.request.type = is_cancel ? Venue::RequestType::CANCEL : Venue::RequestType::NEW;
        wire.request.client_id = request.client_id_;
        wire.request.ticker_id = 0;
        wire.request.client_order_id = request.order_id_;
        wire.request.side = original.side_ == Common::Side::BUY ? Venue::Side::BUY : Venue::Side::SELL;
        wire.request.order_type = original.order_type_ == Common::OrderType::LIMIT ? Venue::OrderType::LIMIT : Venue::OrderType::MARKET;
        wire.request.time_in_force = Venue::TimeInForce::DAY;
        wire.request.price_ticks = original.price_;
        wire.request.qty = original.qty_;
        // The venue stamps receive time from its own realtime clock.
        wire.request.rx_time = 0;
        if (outgoing.size() >= config.capacity) throw std::runtime_error("Simex outbound buffer overflow");
        if (orders.size() >= config.capacity) {
            for (auto it = orders.begin(); it != orders.end();) {
                if (it->second.terminal && !it->second.cancel_pending) it = orders.erase(it);
                else ++it;
            }
        }
        if (!is_cancel) {
            if (orders.size() >= config.capacity) throw std::runtime_error("Simex order metadata capacity exhausted");
            orders.emplace(request.order_id_, Order{request, request.qty_});
        } else found->second.cancel_pending = true;
        if (outgoing.empty()) last_write = Clock::now();
        outgoing.push_back(wire);
    }

    void response(const Wire::WireResponse& wire) {
        checkHeader(wire.header, Wire::MessageKind::RESPONSE, sizeof(wire.response));
        if (wire.header.sequence != response_sequence++) throw std::runtime_error("Simex response sequence gap");
        const auto& value = wire.response;
        if (value.client_id != gateway->clientId() || value.ticker_id != 0)
            throw std::runtime_error("Simex private response identity mismatch");
        const auto found = orders.find(value.client_order_id);
        if (found == orders.end()) throw std::runtime_error("Simex response refers to unknown order");
        auto& order = found->second;
        if (side(value.side) != order.request.side_ || value.position_effect != effect(order.request.offset_))
            throw std::runtime_error("Simex response attributes mismatch");
        Exchange::ClientResponse out;
        out.client_id_ = value.client_id;
        out.ticker_id_ = config.ticker_id;
        out.client_order_id_ = value.client_order_id;
        out.venue_order_id_ = value.market_order_id;
        out.side_ = side(value.side);
        out.price_ = value.price_ticks;
        out.exec_qty_ = value.exec_qty;
        out.leaves_qty_ = value.leaves_qty;
        out.order_type_ = order.request.order_type_;
        out.offset_ = order.request.offset_;
        switch (value.type) {
            case Venue::ResponseType::ACCEPTED: out.type_ = Exchange::ClientResponseType::ACCEPTED; break;
            case Venue::ResponseType::REJECTED: out.type_ = Exchange::ClientResponseType::REJECTED; order.terminal = true; break;
            case Venue::ResponseType::CANCELED: out.type_ = Exchange::ClientResponseType::CANCELED; order.terminal = true; order.cancel_pending = false; break;
            case Venue::ResponseType::CANCEL_REJECTED: out.type_ = Exchange::ClientResponseType::CANCEL_REJECTED; order.cancel_pending = false; break;
            case Venue::ResponseType::FILLED:
                if (order.terminal || !validQty(value.exec_qty) || !validPrice(value.price_ticks) ||
                    value.exec_qty > order.remaining || value.leaves_qty != order.remaining - value.exec_qty)
                    throw std::runtime_error("Invalid simex execution quantities");
                out.type_ = Exchange::ClientResponseType::FILLED;
                order.remaining = value.leaves_qty;
                order.terminal = value.leaves_qty == 0;
                net_position += (out.side_ == Common::Side::BUY ? 1 : -1) * static_cast<std::int64_t>(value.exec_qty);
                if (net_position < INT32_MIN || net_position > INT32_MAX)
                    throw std::runtime_error("Simex position exceeds participant representation");
                break;
            default: throw std::runtime_error("Unsupported simex response kind");
        }
        gateway->publishClientResponse(participant_response_sequence++, out);
        std::clog << "event=simex_response order_id=" << value.client_order_id
                  << " type=" << Exchange::clientResponseTypeToString(out.type_)
                  << " exec_qty=" << value.exec_qty << " leaves_qty=" << value.leaves_qty
                  << " reason=" << simex::common::reasonCodeToString(value.reason) << '\n';
    }

    void apply(const simex::exchange::MarketUpdate& update, bool snapshot = false) {
        if (update.ticker_id != 0) throw std::runtime_error("Unexpected simex instrument");
        (void)side(update.side);
        if (!validPrice(update.price_ticks) ||
            (update.type != Venue::MarketUpdateType::TRADE && update.market_order_id == Venue::INVALID_ORDER_ID))
            throw std::runtime_error("Invalid simex market price or order id");
        if (snapshot) {
            if ((update.type != Venue::MarketUpdateType::ADD && update.type != Venue::MarketUpdateType::MODIFY) ||
                !validQty(update.qty) || !book.emplace(update.market_order_id, update).second)
                throw std::runtime_error("Invalid simex snapshot order");
        } else {
            const auto found = book.find(update.market_order_id);
            switch (update.type) {
                case Venue::MarketUpdateType::ADD:
                    if (!validQty(update.qty) || !book.emplace(update.market_order_id, update).second)
                        throw std::runtime_error("Invalid simex ADD");
                    break;
                case Venue::MarketUpdateType::MODIFY:
                    if (found == book.end() || !validQty(update.leaves_qty))
                        throw std::runtime_error("Invalid simex MODIFY");
                    found->second = update;
                    found->second.qty = update.leaves_qty;
                    break;
                case Venue::MarketUpdateType::CANCEL:
                    if (found == book.end()) throw std::runtime_error("Invalid simex CANCEL");
                    book.erase(found);
                    break;
                case Venue::MarketUpdateType::TRADE:
                    if (!validQty(update.qty)) throw std::runtime_error("Invalid simex TRADE");
                    last_trade = update.price_ticks;
                    break;
                default: throw std::runtime_error("Unsupported simex market kind");
            }
        }
        if (book.size() > config.capacity) throw std::runtime_error("Simex book capacity exceeded");
    }

    void publishBook() {
        std::map<Common::Price, std::uint64_t, std::greater<>> bids;
        std::map<Common::Price, std::uint64_t> asks;
        for (const auto& [id, order] : book) {
            (void)id;
            if (order.side == Venue::Side::BUY) bids[order.price_ticks] += order.qty;
            else asks[order.price_ticks] += order.qty;
        }
        Exchange::MarketUpdate update;
        update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
        update.ticker_id_ = config.ticker_id;
        auto copy = [](const auto& levels, auto& output) {
            std::size_t index = 0;
            for (const auto& [price, qty] : levels) {
                if (qty >= Common::Qty_INVALID) throw std::runtime_error("Simex aggregate quantity overflow");
                output[index++] = {price, static_cast<Common::Qty>(qty)};
                if (index == output.size()) break;
            }
        };
        copy(bids, update.depth_snapshot_.bids_);
        copy(asks, update.depth_snapshot_.asks_);
        update.depth_snapshot_.last_price_ = last_trade;
        // v1 has no session volume, open interest, or historical last trade.
        // TODO(simex-counterparty): Once the market-data contract is available,
        // decide which session statistics belong in snapshots and which are
        // required for Jev readiness. Keep unavailable fields unknown until then.
        update.depth_snapshot_.open_interest_ = std::numeric_limits<double>::quiet_NaN();
        // TODO(simex-counterparty): Use the counterparty's quote cadence to decide
        // whether unchanged quotes expire and when evaluations must pause.
        // Periodic snapshots currently prove transport liveness, not quote freshness.
        const bool ready = !bids.empty() && !asks.empty() && bids.begin()->first < asks.begin()->first;
        if (ready != usable) std::clog << "event=simex_market_state state=" << (ready ? "ready" : "waiting_for_market_data") << '\n';
        usable = ready;
        market->publishMarketUpdate(update);
    }

    void datagram(const std::byte* bytes, std::size_t size) {
        if (size < sizeof(Wire::FrameHeader)) throw std::runtime_error("Truncated simex datagram");
        Wire::FrameHeader header;
        std::memcpy(&header, bytes, sizeof(header));
        if (header.kind == static_cast<std::uint8_t>(Wire::MessageKind::UPDATE)) {
            checkHeader(header, Wire::MessageKind::UPDATE, sizeof(simex::exchange::MarketUpdate));
            if (size != sizeof(Wire::WireUpdate)) throw std::runtime_error("Invalid simex incremental size");
            Wire::WireUpdate wire;
            std::memcpy(&wire, bytes, sizeof(wire));
            if (!synchronized) {
                if (pending.size() >= config.capacity || !pending.emplace(header.sequence, wire.update).second)
                    throw std::runtime_error("Simex startup buffer overflow or duplicate sequence");
                return;
            }
            if (header.sequence <= snapshot_sequence) return;
            if (header.sequence != market_sequence + 1) throw std::runtime_error("Simex market sequence gap");
            apply(wire.update);
            market_sequence = header.sequence;
            publishBook();
        } else if (header.kind == static_cast<std::uint8_t>(Wire::MessageKind::SNAPSHOT)) {
            checkHeader(header, Wire::MessageKind::SNAPSHOT, size - sizeof(header));
            if (size < sizeof(Wire::WireSnapshotPrefix)) throw std::runtime_error("Truncated simex snapshot");
            Wire::WireSnapshotPrefix prefix;
            std::memcpy(&prefix, bytes, sizeof(prefix));
            if (prefix.reserved != 0 || prefix.order_count > config.capacity ||
                size != sizeof(prefix) + static_cast<std::size_t>(prefix.order_count) * sizeof(Wire::WireSnapshotOrder))
                throw std::runtime_error("Invalid simex snapshot size");
            if (synchronized) {
                // A snapshot may precede increments still queued at the publisher.
                // Do not install it, but use its watermark to detect a lost tail.
                advertised_sequence = std::max(advertised_sequence, header.sequence);
                if (advertised_sequence > market_sequence && ahead_since == Clock::time_point{}) ahead_since = Clock::now();
                return;
            }
            for (std::uint32_t i = 0; i < prefix.order_count; ++i) {
                Wire::WireSnapshotOrder order;
                std::memcpy(&order, bytes + sizeof(prefix) + i * sizeof(order), sizeof(order));
                if (order.sequence == 0 || order.sequence > header.sequence) throw std::runtime_error("Invalid snapshot order sequence");
                apply(order.update, true);
            }
            snapshot_sequence = market_sequence = header.sequence;
            for (const auto& [sequence, update] : pending) {
                if (sequence <= market_sequence) continue;
                if (sequence != market_sequence + 1) throw std::runtime_error("Simex startup sequence gap");
                apply(update);
                market_sequence = sequence;
            }
            pending.clear();
            synchronized = true;
            publishBook();
        } else throw std::runtime_error("Unknown simex datagram kind");
    }

    void poll() {
        try {
            if (!market_started) { market->start(); market_started = true; }
            std::array<pollfd, 2> descriptors{{{tcp, static_cast<short>(POLLIN | (outgoing.empty() ? 0 : POLLOUT)), 0}, {udp, POLLIN, 0}}};
            const auto result = ::poll(descriptors.data(), descriptors.size(), 1);
            if (result < 0 && !wouldBlock()) throw std::runtime_error("Simex socket poll failed");
            for (const auto& fd : descriptors) {
                if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) throw std::runtime_error("Simex transport disconnected");
            }
            if (descriptors[0].revents & POLLIN) {
                for (std::size_t batch = 0; batch < 256; ++batch) {
                    const auto count = ::recv(tcp, reinterpret_cast<char*>(&incoming) + received, sizeof(incoming) - received, 0);
                    if (count == 0) throw std::runtime_error("Simex TCP disconnected; execution status may be unknown");
                    if (count < 0) { if (wouldBlock()) break; throw std::runtime_error("Simex TCP read failed"); }
                    received += static_cast<std::size_t>(count);
                    if (received == sizeof(incoming)) { response(incoming); incoming = {}; received = 0; }
                }
            }
            if (descriptors[1].revents & POLLIN) {
                std::array<std::byte, 65536> bytes;
                for (std::size_t batch = 0; batch < 256; ++batch) {
                    sockaddr_in sender{};
                    socklen_t length = sizeof(sender);
                    const auto count = ::recvfrom(udp, bytes.data(), bytes.size(), MSG_TRUNC,
                                                 reinterpret_cast<sockaddr*>(&sender), &length);
                    if (count < 0) { if (wouldBlock()) break; throw std::runtime_error("Simex UDP read failed"); }
                    if (sender.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || count > 65507)
                        throw std::runtime_error("Invalid simex UDP source or size");
                    datagram(bytes.data(), static_cast<std::size_t>(count));
                    last_packet = Clock::now();
                }
            }
            if (!outgoing.empty() && (descriptors[0].revents & POLLOUT)) {
                auto& wire = outgoing.front();
                if (sent == 0) {
                    if (!usable && wire.request.type == Venue::RequestType::NEW) {
                        auto& order = orders.at(wire.request.client_order_id);
                        reject(order.request, "market_became_unusable_before_send");
                        order.terminal = true;
                        outgoing.pop_front();
                        return;
                    }
                    wire.header.sequence = request_sequence;
                }
                const auto count = ::send(tcp, reinterpret_cast<const char*>(&wire) + sent, sizeof(wire) - sent, MSG_NOSIGNAL);
                if (count < 0 && !wouldBlock()) throw std::runtime_error("Simex TCP send failed; execution status may be unknown");
                if (count > 0) { sent += static_cast<std::size_t>(count); last_write = Clock::now(); }
                if (sent == sizeof(wire)) { outgoing.pop_front(); sent = 0; ++request_sequence; }
            }
            if (!outgoing.empty() && Clock::now() - last_write > config.connect_timeout)
                throw std::runtime_error("Simex TCP send timeout; execution status may be unknown");
            if (Clock::now() - last_packet > config.feed_timeout)
                throw std::runtime_error("Simex market-data transport timeout");
            if (market_sequence >= advertised_sequence) ahead_since = {};
            if (ahead_since != Clock::time_point{} && Clock::now() - ahead_since > config.feed_timeout)
                throw std::runtime_error("Simex snapshot confirms missing market increments");
        } catch (const std::exception& error) {
            std::cerr << "event=simex_fatal reason=" << error.what() << std::endl;
            std::terminate();
        }
    }
};

SimexVenueAdapter::SimexVenueAdapter(MarketDataConsumer* market, OrderGateway* orders, SimexVenueConfig config)
    : impl_(std::make_unique<Impl>(market, orders, std::move(config))) {}
SimexVenueAdapter::~SimexVenueAdapter() = default;
auto SimexVenueAdapter::start() -> void { impl_->start(); }
auto SimexVenueAdapter::stop() -> void { impl_->stop(); }
auto SimexVenueAdapter::running() const noexcept -> bool { return impl_->running.load(); }
}  // namespace Trading
