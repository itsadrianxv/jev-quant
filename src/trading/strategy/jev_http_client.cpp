#include "jev_http_client.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifndef JEV_QUANT_HAS_CURL
#define JEV_QUANT_HAS_CURL 0
#endif

#if JEV_QUANT_HAS_CURL
#include <curl/curl.h>
#endif

namespace Trading {
namespace {

auto jsonNumber(double value) -> std::string {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

auto jsonQuote(const std::string& value) -> std::string {
  std::string result = "\"";
  for (const auto character : value) {
    if (character == '\\' || character == '\"') {
      result += '\\';
    }
    result += character;
  }
  result += '\"';
  return result;
}

auto choiceQuestion(const std::string& question, const std::string& criteria)
    -> std::string {
  return "{\"type\":\"choice\",\"instructions\":{\"question\":" +
         jsonQuote(question) + "},\"criteria\":" + criteria + "}";
}

auto extractObject(const std::string& text, const std::string& key)
    -> std::string {
  const auto key_position = text.find(jsonQuote(key));
  if (key_position == std::string::npos) {
    throw std::runtime_error("Missing JSON object: " + key);
  }
  const auto begin = text.find('{', key_position);
  if (begin == std::string::npos) {
    throw std::runtime_error("Malformed JSON object: " + key);
  }

  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (std::size_t index = begin; index < text.size(); ++index) {
    const auto character = text[index];
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '\"') {
        quoted = false;
      }
      continue;
    }
    if (character == '\"') {
      quoted = true;
    } else if (character == '{') {
      ++depth;
    } else if (character == '}' && --depth == 0) {
      return text.substr(begin, index - begin + 1);
    }
  }
  throw std::runtime_error("Unterminated JSON object: " + key);
}

auto extractString(const std::string& text, const std::string& key)
    -> std::string {
  const auto key_position = text.find(jsonQuote(key));
  if (key_position == std::string::npos) {
    throw std::runtime_error("Missing JSON string: " + key);
  }
  const auto colon = text.find(':', key_position);
  const auto begin = text.find('"', colon);
  if (colon == std::string::npos || begin == std::string::npos) {
    throw std::runtime_error("Malformed JSON string: " + key);
  }
  std::string result;
  bool escaped = false;
  for (auto index = begin + 1; index < text.size(); ++index) {
    const auto character = text[index];
    if (escaped) {
      result += character;
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else if (character == '"') {
      return result;
    } else {
      result += character;
    }
  }
  throw std::runtime_error("Unterminated JSON string: " + key);
}

auto extractNumber(const std::string& text, const std::string& key) -> double {
  const auto key_position = text.find(jsonQuote(key));
  if (key_position == std::string::npos) {
    throw std::runtime_error("Missing JSON number: " + key);
  }
  const auto colon = text.find(':', key_position);
  if (colon == std::string::npos) {
    throw std::runtime_error("Malformed JSON number: " + key);
  }
  const auto begin = text.find_first_of("-0123456789.", colon + 1);
  if (begin == std::string::npos) {
    throw std::runtime_error("Malformed JSON number: " + key);
  }
  const auto end = text.find_first_not_of("-+0123456789.eE", begin);
  return std::stod(text.substr(begin, end - begin));
}

auto probability(const std::string& probabilities, const std::string& key)
    -> double {
  const auto key_position = probabilities.find(jsonQuote(key));
  if (key_position == std::string::npos) {
    return 0.0;
  }
  return extractNumber(probabilities.substr(key_position), key);
}

#if JEV_QUANT_HAS_CURL
auto writeResponse(char* data, std::size_t size, std::size_t count,
                   void* user_data) -> std::size_t {
  auto* response = static_cast<std::string*>(user_data);
  response->append(data, size * count);
  return size * count;
}
#endif

}  // namespace

auto JevHttpClient::buildRequestBody(const JevEvaluationState& state) const
    -> std::string {
  const auto& position = state.position_;
  const auto& depth = state.depth_snapshot_;
  const auto intent_criteria = position.net_position_ == 0
                                   ? "{\"open\":\"open a position\",\"hold\":\"take no action\"}"
                                   : "{\"open\":\"increase exposure\",\"close\":\"reduce the current position\",\"hold\":\"take no action\"}";

  std::ostringstream body;
  body << "{\"model\":" << jsonQuote(config_.model_)
       << ",\"state\":{\"evaluation_id\": " << state.evaluation_id_
       << ",\"ticker_id\": " << state.ticker_id_
       << ",\"position\":{\"net\": " << position.net_position_
       << ",\"today\": " << position.today_qty_
       << ",\"yesterday\": " << position.yesterday_qty_
       << ",\"average_entry_price\": " << position.average_entry_price_
       << ",\"realized_pnl\": " << jsonNumber(position.realized_pnl_)
       << ",\"unrealized_pnl\": " << jsonNumber(position.unrealized_pnl_)
       << "},\"working_orders\": [";
  for (std::size_t index = 0; index < state.working_orders_.size(); ++index) {
    if (index != 0) {
      body << ',';
    }
    const auto& order = state.working_orders_[index];
    body << "{\"order_id\": " << order.order_id_
         << ",\"side\": " << jsonQuote(Common::sideToString(order.side_))
         << ",\"price\": " << order.price_
         << ",\"qty\": " << order.qty_
         << ",\"order_type\": "
         << jsonQuote(Common::orderTypeToString(order.order_type_))
         << ",\"offset\": "
         << jsonQuote(Common::orderOffsetToString(order.offset_))
         << ",\"state\": " << jsonQuote(omOrderStateToString(order.state_))
         << "}";
  }
  body << "],\"depth\":{\"last_price\": " << depth.last_price_
       << ",\"bids\":[";
  for (std::size_t index = 0; index < depth.bids_.size(); ++index) {
    if (index != 0) {
      body << ',';
    }
    body << "{\"price\": " << depth.bids_[index].price_
         << ",\"qty\": " << depth.bids_[index].qty_ << "}";
  }
  body << "],\"asks\":[";
  for (std::size_t index = 0; index < depth.asks_.size(); ++index) {
    if (index != 0) {
      body << ',';
    }
    body << "{\"price\": " << depth.asks_[index].price_
         << ",\"qty\": " << depth.asks_[index].qty_ << "}";
  }
  body << "],\"volume\": " << depth.volume_
       << ",\"open_interest\": " << jsonNumber(depth.open_interest_)
       << ",\"upper_limit_price\": " << depth.upper_limit_price_
       << ",\"lower_limit_price\": " << depth.lower_limit_price_
       << "},\"account\":{\"available_margin\": "
       << jsonNumber(state.account_.available_margin_)
       << ",\"used_margin\": " << jsonNumber(state.account_.used_margin_)
       << ",\"equity\": " << jsonNumber(state.account_.equity_)
       << "},\"risk\":{\"max_order_size\": "
       << state.risk_.max_order_size_
       << ",\"max_position\": " << state.risk_.max_position_
       << ",\"max_loss\": " << jsonNumber(state.risk_.max_loss_)
       << "}},\"questions\":{\"bias\":"
       << choiceQuestion("long or short?", "{\"long\":\"long\",\"short\":\"short\"}")
       << ",\"intent\":"
       << choiceQuestion("open, close, or hold?", intent_criteria)
       << "}}";
  return body.str();
}

auto JevHttpClient::parseDecision(const std::string& body,
                                  const JevEvaluationState& state)
    -> JevDecision {
  const auto answers = extractObject(body, "answers");
  const auto bias = extractObject(answers, "bias");
  const auto intent = extractObject(answers, "intent");
  const auto bias_probabilities = extractObject(bias, "probabilities");
  const auto intent_probabilities = extractObject(intent, "probabilities");

  JevDecision decision;
  decision.evaluation_id_ = state.evaluation_id_;
  decision.ticker_id_ = state.ticker_id_;
  const auto bias_choice = extractString(bias, "choice");
  const auto intent_choice = extractString(intent, "choice");
  decision.bias_ = bias_choice == "long" ? JevBias::LONG : JevBias::SHORT;
  if (intent_choice == "open") {
    decision.intent_ = JevIntent::OPEN;
  } else if (intent_choice == "close") {
    decision.intent_ = JevIntent::CLOSE;
  } else {
    decision.intent_ = JevIntent::HOLD;
  }
  decision.long_probability_ = probability(bias_probabilities, "long");
  decision.short_probability_ = probability(bias_probabilities, "short");
  decision.open_probability_ = probability(intent_probabilities, "open");
  decision.close_probability_ = probability(intent_probabilities, "close");
  decision.hold_probability_ = probability(intent_probabilities, "hold");
  decision.confidence_ = extractNumber(intent, "confidence");
  return decision;
}

auto JevHttpClient::evaluate(const JevEvaluationState& state) -> JevDecision {
#if !JEV_QUANT_HAS_CURL
  (void)state;
  throw std::runtime_error(
      "JevHttpClient requires libcurl development headers and library");
#else
  if (config_.api_key_.empty()) {
    throw std::invalid_argument("JevHttpClient API key is empty");
  }
  static std::once_flag curl_init_once;
  std::call_once(curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  auto* curl = curl_easy_init();
  if (curl == nullptr) {
    throw std::runtime_error("curl_easy_init failed");
  }

  std::string response;
  const auto request_body = buildRequestBody(state);
  auto* headers = curl_slist_append(nullptr, "Content-Type: application/json");
  const auto authorization = "Authorization: Bearer " + config_.api_key_;
  headers = curl_slist_append(headers, authorization.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, config_.endpoint_.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(request_body.size()));
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
    throw std::runtime_error(curl_easy_strerror(result));
  }
  if (status < 200 || status >= 300) {
    throw std::runtime_error("Jev HTTP status: " + std::to_string(status) +
                             " body: " + response);
  }
  return parseDecision(response, state);
#endif
}

}  // namespace Trading
