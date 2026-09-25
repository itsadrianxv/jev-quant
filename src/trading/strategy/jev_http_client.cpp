#include "jev_http_client.h"

#include <mutex>
#include <stdexcept>
#include <string>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace Trading {
namespace {

using Json = nlohmann::json;

auto writeResponse(char* data, std::size_t size, std::size_t count,
                                      void* user_data) -> std::size_t {
    auto* response = static_cast<std::string*>(user_data);
    response->append(data, size * count);
    return size * count;
}

auto probability(const Json& probabilities, const char* key) -> double {
    if (!probabilities.contains(key) || !probabilities.at(key).is_number())
        throw std::runtime_error(std::string("Missing probability: ") + key);
    const auto value = probabilities.at(key).get<double>();
    if (value < 0.0 || value > 1.0)
        throw std::runtime_error(std::string("Probability out of range: ") + key);
    return value;
}

auto answer(const Json& answers, const char* key) -> const Json& {
    if (!answers.contains(key) || !answers.at(key).is_object())
        throw std::runtime_error(std::string("Missing answer: ") + key);
    const auto& value = answers.at(key);
    if (!value.contains("type") || value.at("type") != "choice")
        throw std::runtime_error(std::string("Unexpected answer type: ") + key);
    return value;
}

}  // namespace

auto JevHttpClient::buildRequestBody(const JevEvaluationState& state) const
        -> std::string {
    const auto& position = state.position_;
    const auto& depth = state.depth_snapshot_;
    const Json criteria = position.net_position_ == 0
                                                        ? Json{{"open", "open a position"}, {"hold", "take no action"}}
                                                        : Json{{"open", "increase exposure"},
                                                                      {"close", "reduce the current position"},
                                                                      {"hold", "take no action"}};
    Json working_orders = Json::array();
    for (const auto& order : state.working_orders_) {
        working_orders.push_back({{"order_id", order.order_id_},
                                                            {"side", Common::sideToString(order.side_)},
                                                            {"price", order.price_}, {"qty", order.qty_},
                                                            {"order_type", Common::orderTypeToString(order.order_type_)},
                                                            {"offset", Common::orderOffsetToString(order.offset_)},
                                                            {"state", omOrderStateToString(order.state_)}});
    }
    Json bids = Json::array();
    for (const auto& level : depth.bids_)
        bids.push_back({{"price", level.price_}, {"qty", level.qty_}});
    Json asks = Json::array();
    for (const auto& level : depth.asks_)
        asks.push_back({{"price", level.price_}, {"qty", level.qty_}});
    const Json state_json = {
            {"evaluation_id", state.evaluation_id_}, {"ticker_id", state.ticker_id_},
            {"position", {{"net", position.net_position_}, {"today", position.today_qty_},
                                          {"yesterday", position.yesterday_qty_},
                                          {"average_entry_price", position.average_entry_price_},
                                          {"realized_pnl", position.realized_pnl_},
                                          {"unrealized_pnl", position.unrealized_pnl_}}},
            {"working_orders", working_orders},
            {"depth", {{"last_price", depth.last_price_}, {"bids", bids}, {"asks", asks},
                                    {"volume", depth.volume_}, {"open_interest", depth.open_interest_},
                                    {"upper_limit_price", depth.upper_limit_price_},
                                    {"lower_limit_price", depth.lower_limit_price_}}},
            {"account", {{"available_margin", state.account_.available_margin_},
                                        {"used_margin", state.account_.used_margin_},
                                        {"equity", state.account_.equity_}}},
            {"risk", {{"max_order_size", state.risk_.max_order_size_},
                                  {"max_position", state.risk_.max_position_},
                                  {"max_loss", state.risk_.max_loss_}}}};
    const Json request = {
            {"model", config_.model_}, {"state", state_json},
            {"questions", {
                    {"bias", {{"type", "choice"}, {"instructions", "long or short?"},
                                        {"criteria", {{"long", "long"}, {"short", "short"}}}}},
                    {"intent", {{"type", "choice"}, {"instructions", "open, close, or hold?"},
                                            {"criteria", criteria}}}}}};
    return request.dump();
}

auto JevHttpClient::parseDecision(const std::string& body,
                                                                    const JevEvaluationState& state)
        -> JevDecision {
    const auto response = Json::parse(body);
    if (!response.contains("answers") || !response.at("answers").is_object())
        throw std::runtime_error("Response has no answers object");
    const auto& answers = response.at("answers");
    const auto& bias = answer(answers, "bias");
    const auto& intent = answer(answers, "intent");
    if (!bias.contains("choice") || !bias.at("choice").is_string() ||
            !intent.contains("choice") || !intent.at("choice").is_string())
        throw std::runtime_error("Choice answer is missing choice");
    const auto bias_choice = bias.at("choice").get<std::string>();
    const auto intent_choice = intent.at("choice").get<std::string>();
    if (bias_choice != "long" && bias_choice != "short")
        throw std::runtime_error("Unknown bias choice");
    if (intent_choice != "open" && intent_choice != "close" && intent_choice != "hold")
        throw std::runtime_error("Unknown intent choice");
    if (!bias.contains("probabilities") || !intent.contains("probabilities") ||
            !bias.at("probabilities").is_object() || !intent.at("probabilities").is_object() ||
            !bias.contains("confidence") || !intent.contains("confidence") ||
            !bias.at("confidence").is_number() || !intent.at("confidence").is_number())
        throw std::runtime_error("Choice answer is missing probabilities or confidence");
    const auto confidence = intent.at("confidence").get<double>();
    if (confidence < 0.0 || confidence > 1.0)
        throw std::runtime_error("Confidence out of range");
    JevDecision decision;
    decision.evaluation_id_ = state.evaluation_id_;
    decision.ticker_id_ = state.ticker_id_;
    decision.bias_ = bias_choice == "long" ? JevBias::LONG : JevBias::SHORT;
    decision.intent_ = intent_choice == "open" ? JevIntent::OPEN
                                              : intent_choice == "close" ? JevIntent::CLOSE : JevIntent::HOLD;
    decision.long_probability_ = probability(bias.at("probabilities"), "long");
    decision.short_probability_ = probability(bias.at("probabilities"), "short");
    decision.open_probability_ = probability(intent.at("probabilities"), "open");
    decision.close_probability_ = probability(intent.at("probabilities"), "close");
    decision.hold_probability_ = probability(intent.at("probabilities"), "hold");
    decision.confidence_ = confidence;
    return decision;
}

auto JevHttpClient::evaluate(const JevEvaluationState& state) -> JevDecision {
    if (config_.api_key_.empty()) throw std::invalid_argument("Jev API key is empty");
    static std::once_flag curl_init_once;
    std::call_once(curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    const auto request_body = buildRequestBody(state);
    for (unsigned attempt = 0; attempt <= config_.max_retries_; ++attempt) {
        auto* curl = curl_easy_init();
        if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");
        std::string response;
        auto* headers = curl_slist_append(nullptr, "Content-Type: application/json");
        const auto authorization = "Authorization: Bearer " + config_.api_key_;
        headers = curl_slist_append(headers, authorization.c_str());
        if (!config_.http_referer_.empty()) {
            const auto header = "HTTP-Referer: " + config_.http_referer_;
            headers = curl_slist_append(headers, header.c_str());
        }
        if (!config_.http_title_.empty()) {
            const auto header = "X-Title: " + config_.http_title_;
            headers = curl_slist_append(headers, header.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_URL, config_.endpoint_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_body.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, config_.timeout_ms_);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, config_.timeout_ms_);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const auto result = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK) {
            if (attempt < config_.max_retries_) continue;
            throw std::runtime_error(curl_easy_strerror(result));
        }
        if (status < 200 || status >= 300)
            throw std::runtime_error("Jev HTTP status: " + std::to_string(status));
        return parseDecision(response, state);
    }
    throw std::runtime_error("Jev request failed");
}

}  // namespace Trading
