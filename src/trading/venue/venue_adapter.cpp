#include "trading/venue/venue_adapter.h"

#include <algorithm>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <chrono>

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Trading {
namespace {
auto trim(std::string value) -> std::string {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
auto required(const std::unordered_map<std::string, std::string>& values,
              std::string_view key) -> std::string {
    const auto it = values.find(std::string(key));
    if (it == values.end() || it->second.empty()) {
        throw std::runtime_error("Missing .env value: " + std::string(key));
    }
    return it->second;
}
auto writeBody(char* data, std::size_t size, std::size_t count, void* target) -> std::size_t {
    auto* body = static_cast<std::string*>(target);
    body->append(data, size * count);
    return size * count;
}
auto hmacSha256(const std::string& payload, const std::string& secret) -> std::string {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
         digest, &length);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 0x0f]);
    }
    return result;
}
auto signedRequest(const std::string& method, const std::string& path,
                   const std::vector<std::pair<std::string, std::string>>& params,
                   const BinanceUmFuturesConfig& config) -> nlohmann::json {
    std::string query;
    for (const auto& [key, value] : params) {
        if (!query.empty()) query += '&';
        query += key + '=' + value;
    }
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    query += "&timestamp=" + std::to_string(timestamp);
    query += "&signature=" + hmacSha256(query, config.secret_key);
    auto* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl initialization failed");
    std::string body;
    const auto url = std::string("https://demo-fapi.binance.com") + path + "?" + query;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + config.api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) throw std::runtime_error(curl_easy_strerror(result));
    if (status < 200 || status >= 300) throw std::runtime_error("Binance REST HTTP " + std::to_string(status) + ": " + body);
    return nlohmann::json::parse(body);
}
auto createListenKey(const BinanceUmFuturesConfig& config) -> std::string {
    auto* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl initialization failed");
    std::string body;
    const auto url = std::string("https://demo-fapi.binance.com/fapi/v1/listenKey");
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + config.api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300)
        throw std::runtime_error("listenKey creation failed");
    return nlohmann::json::parse(body).at("listenKey").get<std::string>();
}
auto keepListenKey(const BinanceUmFuturesConfig& config, const std::string& key) -> void {
    auto* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl initialization failed");
    const auto url = std::string("https://demo-fapi.binance.com/fapi/v1/listenKey?listenKey=") + key;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + config.api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300)
        throw std::runtime_error("listenKey keepalive failed");
}
auto fetchDepth(const std::string& symbol) -> nlohmann::json {
    auto* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl initialization failed");
    std::string body;
    const auto url = std::string("https://demo-fapi.binance.com/fapi/v1/depth?symbol=") + symbol + "&limit=1000";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) throw std::runtime_error(std::string("depth request failed: ") + curl_easy_strerror(result));
    if (status != 200) throw std::runtime_error("depth returned HTTP " + std::to_string(status));
    return nlohmann::json::parse(body);
}
auto publishBook(const std::map<double, double>& bids,
                  const std::map<double, double>& asks,
                  Common::TickerId ticker_id, MarketDataConsumer* consumer) -> void {
    Exchange::MarketUpdate update;
    update.type_ = Exchange::MarketUpdateType::DEPTH_SNAPSHOT;
    update.ticker_id_ = ticker_id;
    std::size_t index = 0;
    for (auto it = bids.rbegin(); it != bids.rend() &&
         index < Common::DEPTH_SNAPSHOT_LEVELS; ++it, ++index) {
        update.depth_snapshot_.bids_[index].price_ =
                static_cast<Common::Price>(it->first * 100000000.0);
        update.depth_snapshot_.bids_[index].qty_ =
                static_cast<Common::Qty>(it->second);
    }
    index = 0;
    for (auto it = asks.begin(); it != asks.end() &&
         index < Common::DEPTH_SNAPSHOT_LEVELS; ++it, ++index) {
        update.depth_snapshot_.asks_[index].price_ =
                static_cast<Common::Price>(it->first * 100000000.0);
        update.depth_snapshot_.asks_[index].qty_ =
                static_cast<Common::Qty>(it->second);
    }
    consumer->publishMarketUpdate(update);
}
auto validateExchangeInfo(const std::string& symbol) -> void {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl initialization failed");
    std::string body;
    const auto url = std::string("https://demo-fapi.binance.com/fapi/v1/exchangeInfo");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) throw std::runtime_error(std::string("exchangeInfo request failed: ") + curl_easy_strerror(result));
    if (status != 200) throw std::runtime_error("exchangeInfo returned HTTP " + std::to_string(status));
    const auto document = nlohmann::json::parse(body);
    for (const auto& item : document.at("symbols")) {
        if (item.value("symbol", "") == symbol) {
            if (item.value("status", "") != "TRADING") throw std::runtime_error("Configured Binance symbol is not TRADING");
            if (!item.contains("filters")) throw std::runtime_error("exchangeInfo has no filters");
            return;
        }
    }
    throw std::runtime_error("Configured Binance symbol was not found in exchangeInfo");
}
auto validateAccountConfiguration(const BinanceUmFuturesConfig& config) -> void {
    const auto mode = signedRequest("GET", "/fapi/v1/positionSide/dual", {}, config);
    if (mode.value("dualSidePosition", true)) {
        throw std::runtime_error("Binance account must use one-way position mode");
    }
    const auto risk = signedRequest(
            "GET", "/fapi/v2/positionRisk", {{"symbol", config.symbol}}, config);
    if (!risk.is_array() || risk.empty()) {
        throw std::runtime_error("Binance position risk response is empty");
    }
    const auto& position = risk.front();
    const auto margin_type = position.value("marginType", "");
    const auto leverage = position.value("leverage", "");
    if (margin_type != config.margin_type) {
        throw std::runtime_error("Binance margin type does not match configuration");
    }
    if (leverage != std::to_string(config.leverage)) {
        throw std::runtime_error("Binance leverage does not match configuration");
    }
}
auto sideString(Common::Side side) -> std::string {
    if (side == Common::Side::BUY) return "BUY";
    if (side == Common::Side::SELL) return "SELL";
    throw std::invalid_argument("Invalid order side");
}
}  // namespace

BinanceUmFuturesVenueAdapter::BinanceUmFuturesVenueAdapter(
        MarketDataConsumer* market_data, OrderGateway* order_gateway,
        BinanceUmFuturesConfig config)
    : market_data_(market_data), order_gateway_(order_gateway),
      config_(std::move(config)) {}

auto BinanceUmFuturesVenueAdapter::loadConfigFromEnv(
        const std::string& path, Common::TickerId ticker_id)
        -> BinanceUmFuturesConfig {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open Binance .env: " + path);
    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        values[trim(line.substr(0, separator))] =
                trim(line.substr(separator + 1));
    }

    BinanceUmFuturesConfig config;
    config.api_key = required(values, "BINANCE_UM_FUTURES_API_KEY");
    config.secret_key = required(values, "BINANCE_UM_FUTURES_SECRET_KEY");
    config.symbol = required(values, "BINANCE_UM_FUTURES_SYMBOLS");
    if (config.symbol.find(',') != std::string::npos) {
        throw std::runtime_error("The first Binance adapter supports one symbol");
    }
    config.margin_type = required(values, "BINANCE_UM_FUTURES_MARGIN_TYPE");
    config.leverage = static_cast<std::uint32_t>(
            std::stoul(required(values, "BINANCE_UM_FUTURES_LEVERAGE")));
    if (config.leverage == 0) throw std::runtime_error("Leverage must be positive");
    config.ticker_id = ticker_id;
    return config;
}

auto BinanceUmFuturesVenueAdapter::mapOrder(
        const Exchange::ClientRequest& request,
        const BinanceUmFuturesConfig& config) -> BinanceOrderParameters {
    if (request.ticker_id_ != config.ticker_id) {
        throw std::invalid_argument("Request ticker does not match Binance symbol");
    }
    if (request.type_ != Exchange::ClientRequestType::NEW) {
        throw std::invalid_argument("Only NEW requests can be mapped as orders");
    }
    BinanceOrderParameters result;
    result.symbol = config.symbol;
    result.side = sideString(request.side_);
    result.type = Common::orderTypeToString(request.order_type_);
    result.quantity = std::to_string(request.qty_);
    result.price = std::to_string(request.price_);
    result.time_in_force = request.order_type_ == Common::OrderType::LIMIT ? "GTC" : "";
    result.reduce_only = request.offset_ != Common::OrderOffset::OPEN;
    result.new_client_order_id = std::to_string(request.order_id_);
    if (request.order_type_ != Common::OrderType::LIMIT &&
        request.order_type_ != Common::OrderType::MARKET) {
        throw std::invalid_argument("Unsupported Binance order type");
    }
    if (request.qty_ == Common::Qty_INVALID) {
        throw std::invalid_argument("Invalid order quantity");
    }
    return result;
}

auto BinanceUmFuturesVenueAdapter::mapOrderStatus(const std::string& status)
        -> Exchange::ClientResponseType {
    if (status == "FILLED") return Exchange::ClientResponseType::FILLED;
    if (status == "CANCELED" || status == "EXPIRED" ||
        status == "EXPIRED_IN_MATCH") return Exchange::ClientResponseType::CANCELED;
    if (status == "PARTIALLY_FILLED" || status == "NEW")
        return Exchange::ClientResponseType::ACCEPTED;
    return Exchange::ClientResponseType::REJECTED;
}

auto BinanceUmFuturesVenueAdapter::start() -> void {
    if (market_data_ == nullptr || order_gateway_ == nullptr) {
        throw std::runtime_error("Binance adapter requires market data and order gateway");
    }
    if (config_.symbol.empty() || config_.ticker_id == Common::TickerId_INVALID) {
        throw std::runtime_error("Binance adapter configuration is incomplete");
    }
    validateExchangeInfo(config_.symbol);
    validateAccountConfiguration(config_);
    const auto depth = fetchDepth(config_.symbol);
    {
        std::lock_guard lock(book_mutex_);
        for (const auto& level : depth.at("bids")) {
            bids_[std::stod(level[0].get<std::string>())] =
                    std::stod(level[1].get<std::string>());
        }
        for (const auto& level : depth.at("asks")) {
            asks_[std::stod(level[0].get<std::string>())] =
                    std::stod(level[1].get<std::string>());
        }
        last_depth_update_id_ = depth.at("lastUpdateId").get<std::uint64_t>();
        publishBook(bids_, asks_, config_.ticker_id, market_data_);
    }
    depth_stream_ = std::make_unique<BinanceWebSocketStream>(
            config_.symbol,
            [this](const nlohmann::json& message) {
                if (!message.contains("U") || !message.contains("u") ||
                    !message.contains("pu") || !message.contains("b") ||
                    !message.contains("a")) {
                    throw std::runtime_error("Invalid Binance depth event");
                }
                const auto first = message.at("U").get<std::uint64_t>();
                const auto final = message.at("u").get<std::uint64_t>();
                const auto previous = message.at("pu").get<std::uint64_t>();
                std::lock_guard lock(book_mutex_);
                if (final < last_depth_update_id_) return;
                if (last_depth_update_id_ != 0 &&
                    previous != last_depth_update_id_ &&
                    !(first <= last_depth_update_id_ + 1 && final >= last_depth_update_id_ + 1)) {
                    throw std::runtime_error("Binance depth sequence gap");
                }
                for (const auto& level : message.at("b")) {
                    const auto price = std::stod(level[0].get<std::string>());
                    const auto quantity = std::stod(level[1].get<std::string>());
                    if (quantity == 0.0) bids_.erase(price); else bids_[price] = quantity;
                }
                for (const auto& level : message.at("a")) {
                    const auto price = std::stod(level[0].get<std::string>());
                    const auto quantity = std::stod(level[1].get<std::string>());
                    if (quantity == 0.0) asks_.erase(price); else asks_[price] = quantity;
                }
                last_depth_update_id_ = final;
                publishBook(bids_, asks_, config_.ticker_id, market_data_);
            },
            [this](const std::string& error) {
                running_.store(false, std::memory_order_release);
                std::clog << "Binance depth stream failed: " << error << std::endl;
                std::terminate();
            });
    depth_stream_->start();
    listen_key_ = createListenKey(config_);
    user_stream_ = std::make_unique<BinanceWebSocketStream>(
            "/ws/" + listen_key_,
            [this](const nlohmann::json& message) {
                const auto event = message.value("e", "");
                if (event == "listenKeyExpired") {
                    running_.store(false, std::memory_order_release);
                    std::terminate();
                }
                if (event == "ORDER_TRADE_UPDATE") {
                    const auto& order = message.at("o");
                    const auto client_order_id = static_cast<Common::OrderId>(
                            std::stoull(order.value("c", "0")));
                    const auto found = live_orders_.find(client_order_id);
                    if (found == live_orders_.end()) return;
                    const auto cumulative = static_cast<Common::Qty>(
                            std::stod(order.value("z", "0")));
                    auto& previous = cumulative_exec_qty_[client_order_id];
                    if (cumulative <= previous) return;
                    previous = cumulative;
                    Exchange::ClientResponse response;
                    response.type_ = mapOrderStatus(order.value("X", "NEW"));
                    response.client_id_ = found->second.client_id_;
                    response.ticker_id_ = found->second.ticker_id_;
                    response.client_order_id_ = client_order_id;
                    response.venue_order_id_ = order.value("i", Common::OrderId_INVALID);
                    response.side_ = found->second.side_;
                    response.price_ = found->second.price_;
                    response.exec_qty_ = cumulative;
                    response.leaves_qty_ = found->second.qty_ > cumulative
                            ? found->second.qty_ - cumulative : 0;
                    response.order_type_ = found->second.order_type_;
                    response.offset_ = found->second.offset_;
                    order_gateway_->publishClientResponse(
                            order_gateway_->nextExpectedResponseSequence(), response);
                    if (response.type_ == Exchange::ClientResponseType::FILLED ||
                        response.type_ == Exchange::ClientResponseType::CANCELED ||
                        response.type_ == Exchange::ClientResponseType::REJECTED) {
                        live_orders_.erase(found);
                    }
                    return;
                }
                if (event == "ACCOUNT_UPDATE") { account_state_ = message; return; }
            },
            [this](const std::string&) {
                running_.store(false, std::memory_order_release);
                std::terminate();
            });
    user_stream_->start();
    keepalive_running_.store(true, std::memory_order_release);
    keepalive_thread_ = std::thread([this] {
        for (int tick = 0; keepalive_running_.load(std::memory_order_acquire); ++tick) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (tick < 1799 || !keepalive_running_.load(std::memory_order_acquire)) continue;
            try {
                keepListenKey(config_, listen_key_);
                tick = 0;
            } catch (const std::exception& error) {
                std::clog << "Binance listen-key keepalive failed: "
                          << error.what() << std::endl;
                std::terminate();
            }
        }
    });
    order_gateway_->setRequestHandler(
            [this](std::size_t sequence, const Exchange::ClientRequest& request) {
                handleRequest(sequence, request);
            });
    order_gateway_->start();
    market_data_->start();
    running_.store(true, std::memory_order_release);
}

auto BinanceUmFuturesVenueAdapter::stop() -> void {
    running_.store(false, std::memory_order_release);
    if (market_data_ != nullptr) market_data_->stop();
    if (order_gateway_ != nullptr) order_gateway_->stop();
}

auto BinanceUmFuturesVenueAdapter::running() const noexcept -> bool {
    return running_.load(std::memory_order_acquire);
}

auto BinanceUmFuturesVenueAdapter::handleRequest(
        std::size_t sequence, const Exchange::ClientRequest& request) -> void {
    try {
        if (request.type_ == Exchange::ClientRequestType::CANCEL) {
            const auto response = signedRequest(
                    "DELETE", "/fapi/v1/order",
                    {{"symbol", config_.symbol},
                     {"origClientOrderId", std::to_string(request.order_id_)}},
                    config_);
            Exchange::ClientResponse mapped;
            mapped.type_ = mapOrderStatus(response.value("status", "CANCELED"));
            mapped.client_id_ = request.client_id_;
            mapped.ticker_id_ = request.ticker_id_;
            mapped.client_order_id_ = request.order_id_;
            mapped.venue_order_id_ = response.value("orderId", Common::OrderId_INVALID);
            mapped.side_ = request.side_;
            mapped.price_ = request.price_;
            mapped.exec_qty_ = 0;
            mapped.leaves_qty_ = request.qty_;
            mapped.order_type_ = request.order_type_;
            mapped.offset_ = request.offset_;
            order_gateway_->publishClientResponse(sequence, mapped);
            return;
        }
        const auto order = mapOrder(request, config_);
        std::vector<std::pair<std::string, std::string>> params{
                {"symbol", order.symbol}, {"side", order.side},
                {"type", order.type}, {"quantity", order.quantity},
                {"newClientOrderId", order.new_client_order_id}};
        if (order.type == "LIMIT") {
            params.emplace_back("timeInForce", order.time_in_force);
            params.emplace_back("price", order.price);
        }
        if (order.reduce_only) params.emplace_back("reduceOnly", "true");
        const auto response = signedRequest("POST", "/fapi/v1/order", params, config_);
        Exchange::ClientResponse mapped;
        mapped.type_ = Exchange::ClientResponseType::ACCEPTED;
        mapped.client_id_ = request.client_id_;
        mapped.ticker_id_ = request.ticker_id_;
        mapped.client_order_id_ = request.order_id_;
        mapped.venue_order_id_ = response.value("orderId", Common::OrderId_INVALID);
        mapped.side_ = request.side_;
        mapped.price_ = request.price_;
        mapped.exec_qty_ = 0;
        mapped.leaves_qty_ = request.qty_;
        mapped.order_type_ = request.order_type_;
        mapped.offset_ = request.offset_;
        live_orders_[request.order_id_] = request;
        order_gateway_->publishClientResponse(sequence, mapped);
    } catch (const std::exception& error) {
        publishRejected(sequence, request, error.what());
    }
}

auto BinanceUmFuturesVenueAdapter::publishRejected(
        std::size_t sequence, const Exchange::ClientRequest& request,
        const std::string&) -> void {
    Exchange::ClientResponse response;
    response.type_ = request.type_ == Exchange::ClientRequestType::CANCEL
                           ? Exchange::ClientResponseType::CANCEL_REJECTED
                           : Exchange::ClientResponseType::REJECTED;
    response.client_id_ = request.client_id_;
    response.ticker_id_ = request.ticker_id_;
    response.client_order_id_ = request.order_id_;
    response.side_ = request.side_;
    response.price_ = request.price_;
    response.exec_qty_ = 0;
    response.leaves_qty_ = request.qty_;
    response.order_type_ = request.order_type_;
    response.offset_ = request.offset_;
    order_gateway_->publishClientResponse(sequence, response);
}

}  // namespace Trading
















