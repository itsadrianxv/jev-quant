#include "trading/venue/binance_websocket_stream.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/ssl.h>
#include <sys/socket.h>
namespace Trading {
BinanceWebSocketStream::BinanceWebSocketStream(std::string symbol, MessageHandler on_message, ErrorHandler on_error)
    : target_(std::move(symbol)), on_message_(std::move(on_message)), on_error_(std::move(on_error)) {}
BinanceWebSocketStream::~BinanceWebSocketStream() { stop(); }
auto BinanceWebSocketStream::start() -> void {
    if (worker_.joinable()) throw std::runtime_error("WebSocket stream already started");
    stopping_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(state_mutex_);
        connected_ = false;
    }
    worker_ = std::thread(&BinanceWebSocketStream::run, this);
}
auto BinanceWebSocketStream::waitUntilConnected(std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lock(state_mutex_);
    return state_changed_.wait_for(lock, timeout, [this] {
        return connected_ || stopping_.load(std::memory_order_acquire);
    }) && connected_;
}
auto BinanceWebSocketStream::stop() noexcept -> void {
    stopping_.store(true, std::memory_order_release);
    const auto fd = socket_fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    state_changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
auto BinanceWebSocketStream::run() -> void {
    try {
        namespace asio = boost::asio;
        namespace beast = boost::beast;
        asio::io_context io;
        asio::ssl::context ssl(asio::ssl::context::tls_client);
        ssl.set_verify_mode(asio::ssl::verify_none);
        asio::ip::tcp::resolver resolver(io);
        beast::websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(io, ssl);
        const auto results = resolver.resolve("demo-fstream.binance.com", "443");
        beast::get_lowest_layer(ws).connect(results);
        socket_fd_.store(static_cast<int>(
                beast::get_lowest_layer(ws).socket().native_handle()),
                std::memory_order_release);
        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), "demo-fstream.binance.com"))
            throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()));
        ws.next_layer().handshake(asio::ssl::stream_base::client);
        ws.handshake("demo-fstream.binance.com", target_);
        {
            std::lock_guard lock(state_mutex_);
            connected_ = true;
        }
        state_changed_.notify_all();
        while (!stopping_.load(std::memory_order_acquire)) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            const auto message = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
            if (on_message_) on_message_(message);
        }
        beast::error_code ignored;
        ws.close(beast::websocket::close_code::normal, ignored);
        socket_fd_.store(-1, std::memory_order_release);
    } catch (const std::exception& error) {
        socket_fd_.store(-1, std::memory_order_release);
        if (!stopping_.load(std::memory_order_acquire) && on_error_) on_error_(error.what());
    }
}
}


