#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include "trading/venue/simex/simex_venue_adapter.h"
#include "transport/protocol.h"

namespace {
using namespace std::chrono_literals;
namespace Wire = simex::transport;

class SimexAdapter : public ::testing::Test {
  protected:
    void SetUp() override {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sender_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(listener_, 0);
        ASSERT_GE(sender_, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(0, ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
        ASSERT_EQ(0, ::listen(listener_, 1));
        socklen_t size = sizeof(address);
        ASSERT_EQ(0, ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size));
        Trading::SimexVenueConfig config;
        config.tcp_port = ntohs(address.sin_port);
        config.udp_port = 0; // Ask the OS for an unused UDP port via a temporary socket.
        const int probe = ::socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(probe, 0);
        address.sin_port = 0;
        const int bound = ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        const int named = ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size);
        ::close(probe);
        ASSERT_EQ(0, bound);
        ASSERT_EQ(0, named);
        config.udp_port = ntohs(address.sin_port);
        destination_ = address;
        config.feed_timeout = 30s;
        adapter_ = std::make_unique<Trading::SimexVenueAdapter>(&market_, &gateway_, config);
        adapter_->prepare();
        EXPECT_FALSE(market_.running());
        EXPECT_FALSE(gateway_.running());
        market_.start();
        gateway_.start();
        adapter_->waitUntilReady();
        pollfd descriptor{listener_, POLLIN, 0};
        ASSERT_EQ(1, ::poll(&descriptor, 1, 2000));
        peer_ = ::accept(listener_, nullptr, nullptr);
        ASSERT_GE(peer_, 0);
    }

    void TearDown() override {
        stopAdapter();
        for (const int fd : {peer_, listener_, sender_}) if (fd >= 0) ::close(fd);
    }

    void stopAdapter() {
        market_.stop();
        if (adapter_) adapter_->quiesce();
        gateway_.stop();
        if (adapter_) adapter_->stop();
    }

    void snapshot() {
        Wire::WireSnapshotPrefix snapshot;
        snapshot.header.kind = static_cast<std::uint8_t>(Wire::MessageKind::SNAPSHOT);
        snapshot.header.payload_size = sizeof(snapshot) - sizeof(snapshot.header);
        const auto sent = ::sendto(sender_, &snapshot, sizeof(snapshot), 0,
                                  reinterpret_cast<sockaddr*>(&destination_), sizeof(destination_));
        ASSERT_EQ(static_cast<ssize_t>(sizeof(snapshot)), sent);
    }

    void sendOrder(Common::OrderId id, Common::Qty qty) {
        Exchange::ClientRequest request;
        request.type_ = Exchange::ClientRequestType::NEW;
        request.client_id_ = 7;
        request.ticker_id_ = 0;
        request.order_id_ = id;
        request.side_ = Common::Side::BUY;
        request.order_type_ = Common::OrderType::LIMIT;
        request.offset_ = Common::OrderOffset::OPEN;
        request.price_ = 100;
        request.qty_ = qty;
        *requests_.getNextToWriteTo() = request;
        requests_.updateWriteIndex();
    }

    template<class Predicate> bool waitFor(Predicate ready) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!ready()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    Wire::WireRequest receiveOrder() {
        Wire::WireRequest result;
        std::size_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (received < sizeof(result)) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Timed out waiting for a Simex TCP order");
            pollfd descriptor{peer_, POLLIN, 0};
            if (::poll(&descriptor, 1, 10) <= 0) continue;
            const auto count = ::recv(peer_, reinterpret_cast<char*>(&result) + received,
                                      sizeof(result) - received, MSG_DONTWAIT);
            if (count <= 0) throw std::runtime_error("Failed to receive Simex TCP order");
            received += static_cast<std::size_t>(count);
        }
        return result;
    }

    int listener_ = -1, sender_ = -1, peer_ = -1;
    sockaddr_in destination_{};
    Exchange::MarketUpdateLFQueue updates_{32};
    Exchange::ClientRequestLFQueue requests_{32};
    Exchange::ClientResponseLFQueue responses_{32};
    Trading::MarketDataConsumer market_{7, &updates_};
    Trading::OrderGateway gateway_{7, &requests_, &responses_};
    std::unique_ptr<Trading::SimexVenueAdapter> adapter_;
};

TEST_F(SimexAdapter, ReceivesMarketSnapshotsWithTheGatewayStopped) {
    gateway_.stop();
    snapshot();
    ASSERT_TRUE(waitFor([&] { return updates_.size() == 1; })) << "marketUpdates=" << updates_.size();
    EXPECT_EQ(Exchange::MarketUpdateType::DEPTH_SNAPSHOT, updates_.getNextToRead()->type_);
    EXPECT_TRUE(market_.running());
    stopAdapter();
    EXPECT_FALSE(market_.running());
}

TEST_F(SimexAdapter, SendsOrdersWithUnusableDepthAndKeepsOtherValidation) {
    snapshot(); // Empty book: neither bid nor ask is usable.
    ASSERT_TRUE(waitFor([&] { return updates_.size() == 1; })) << "marketUpdates=" << updates_.size();
    sendOrder(1, 0);
    ASSERT_TRUE(waitFor([&] { return responses_.size() == 1; })) << "responses=" << responses_.size();
    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, responses_.getNextToRead()->type_);
    responses_.updateReadIndex();
    sendOrder(2, 1);
    const auto wire = receiveOrder();
    EXPECT_EQ(static_cast<std::uint8_t>(Wire::MessageKind::REQUEST), wire.header.kind);
    EXPECT_EQ(2U, wire.request.client_order_id);
    EXPECT_EQ(1, wire.request.qty);
    sendOrder(2, 1);
    ASSERT_TRUE(waitFor([&] { return responses_.size() == 1; })) << "responses=" << responses_.size();
    EXPECT_EQ(Exchange::ClientResponseType::REJECTED, responses_.getNextToRead()->type_);
}
} // namespace
