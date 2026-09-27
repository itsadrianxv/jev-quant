#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "trading/strategy/jev_capture.h"
#include "trading/strategy/jev_decision.h"
#include "trading/strategy/jev_http_client.h"
#include "tools/jev_variant.h"

namespace {

using Json = nlohmann::json;
using Trading::JevHttpCapture;

// Matches the live configuration's X-Title so replays use the same headers.
constexpr auto kReplayHttpTitle = "jev-quant";

auto appendResponse(char* data, std::size_t size, std::size_t count, void* user_data)
        -> std::size_t {
    auto* response = static_cast<std::string*>(user_data);
    response->append(data, size * count);
    return size * count;
}

auto writeTextFile(const std::filesystem::path& path, std::string_view bytes) -> void {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot write " + path.string());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot write " + path.string());
}

auto parseArguments(int argc, char** argv) -> Json {
    Json arguments;
    arguments["variant"] = "current";
    arguments["parse_only"] = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        const auto next = [&]() -> std::string {
            if (index + 1 >= argc)
                throw std::runtime_error("missing value for " + std::string(option));
            return argv[++index];
        };
        if (option == "--capture") arguments["capture"] = next();
        else if (option == "--out") arguments["out"] = next();
        else if (option == "--label") arguments["label"] = next();
        else if (option == "--variant") arguments["variant"] = next();
        else if (option == "--endpoint") arguments["endpoint"] = next();
        else if (option == "--model") arguments["model"] = next();
        else if (option == "--timeout-ms") arguments["timeout_ms"] = std::stol(next());
        else if (option == "--parse-only") arguments["parse_only"] = true;
        else throw std::runtime_error("unknown option " + std::string(option));
    }
    if (!arguments.contains("capture") || !arguments.contains("out") ||
        !arguments.contains("label")) {
        throw std::runtime_error(
                "usage: jev_replay --capture <attempt-dir> --out <dir> --label <name> "
                "[--variant current|previous] [--endpoint URL] [--model M] [--timeout-ms N] "
                "[--parse-only]");
    }
    const auto variant = arguments.at("variant").get<std::string>();
    if (variant != "current" && variant != "previous") {
        throw std::runtime_error("variant must be current or previous");
    }
    return arguments;
}

auto readRequiredFile(const std::filesystem::path& path) -> std::string {
    const auto bytes = Trading::readFileBytes(path);
    if (bytes.empty()) throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

auto readOptionalJson(const std::filesystem::path& path) -> Json {
    const auto bytes = Trading::readFileBytes(path);
    if (bytes.empty()) return Json::object();
    return Json::parse(bytes);
}

auto rawAnswerReport(const Json& answers, const char* key) -> Json {
    if (!answers.contains(key) || !answers.at(key).is_object()) {
        return {{"error", std::string("missing answer ") + key}};
    }
    const auto& value = answers.at(key);
    return {{"type", value.value("type", "")},
                      {"choice", value.value("choice", "")},
                      {"probabilities", value.value("probabilities", Json::object())},
                      {"confidence", value.contains("confidence")
                                                                         ? Json(value.at("confidence"))
                                                                         : Json(nullptr)}};
}

auto criteriaKeys(const Json& request_document) -> Json {
    Json keys = Json::array();
    for (const auto& item : request_document.at("questions").at("intent").at("criteria").items()) {
        keys.push_back(item.key());
    }
    return keys;
}

/// Diagnostics over a raw answer map: probability sums, ranges, and option
/// membership.  Raw data is reported as received, never renormalized.
auto diagnosticChecks(const Json& raw_answers, const Json& request_document) -> Json {
    Json checks = Json::object();
    for (const char* key : {"bias", "intent"}) {
        const auto answer = rawAnswerReport(raw_answers, key);
        Json sum = nullptr;
        Json in_range = nullptr;
        Json options_seen = Json::array();
        if (answer.contains("probabilities") && answer.at("probabilities").is_object()) {
            double total = 0.0;
            in_range = true;
            for (const auto& item : answer.at("probabilities").items()) {
                options_seen.push_back(item.key());
                if (!item.value().is_number()) {
                    in_range = false;
                    continue;
                }
                total += item.value().get<double>();
                if (item.value().get<double>() < 0.0 || item.value().get<double>() > 1.0) {
                    in_range = false;
                }
            }
            sum = total;
            std::sort(options_seen.begin(), options_seen.end());
        }
        Json options_expected;
        if (std::string_view(key) == "intent") {
            const auto net = request_document.at("state").at("position").value("net", 0);
            options_expected = net == 0 ? Json{"open", "hold"} : Json{"open", "close", "hold"};
        } else {
            options_expected = Json{"long", "short"};
        }
        std::sort(options_expected.begin(), options_expected.end());
        checks[key] = {{"probability_sum", sum},
                                     {"probabilities_in_range", in_range},
                                     {"options_seen", options_seen},
                                     {"options_expected", options_expected},
                                     {"options_match", options_seen == options_expected}};
    }
    return checks;
}

auto decisionReport(const std::optional<Trading::JevDecision>& decision) -> Json {
    if (!decision.has_value()) return nullptr;
    const auto intent = [](Trading::JevIntent value) {
        switch (value) {
        case Trading::JevIntent::OPEN: return "open";
        case Trading::JevIntent::CLOSE: return "close";
        case Trading::JevIntent::HOLD: return "hold";
        default: return "invalid";
        }
    };
    const auto bias = [](Trading::JevBias value) {
        switch (value) {
        case Trading::JevBias::LONG: return "long";
        case Trading::JevBias::SHORT: return "short";
        default: return "invalid";
        }
    };
    return {{"intent", intent(decision->intent_)},
                      {"bias", bias(decision->bias_)},
                      {"open_probability", decision->open_probability_},
                      {"close_probability", decision->close_probability_},
                      {"hold_probability", decision->hold_probability_},
                      {"confidence", decision->confidence_}};
}

auto evaluationStateFromRequest(const Json& request_document) -> Trading::JevEvaluationState {
    Trading::JevEvaluationState state;
    const auto& request_state = request_document.at("state");
    state.evaluation_id_ = request_state.value("evaluation_id", std::uint64_t{0});
    state.ticker_id_ = request_state.value("ticker_id", std::uint64_t{0});
    state.position_.net_position_ = request_state.at("position").value("net", 0);
    return state;
}

struct DecodedResponse {
    Json raw_answers = nullptr;
    std::optional<Trading::JevDecision> decision;
    std::string parse_outcome;
    std::string parse_error;
};

/// Decodes a raw response body with the real C++ parser from jev_http_client.
auto decodeResponse(const std::string& response_bytes, const Trading::JevEvaluationState& state)
        -> DecodedResponse {
    DecodedResponse decoded;
    if (response_bytes.empty()) {
        decoded.parse_outcome = "failed";
        decoded.parse_error = "empty response";
        return decoded;
    }
    try {
        decoded.raw_answers = Json::parse(response_bytes).at("answers");
    } catch (const std::exception& error) {
        decoded.parse_outcome = "failed";
        decoded.parse_error = std::string("response is not a valid answers document: ") +
                                                    error.what();
        return decoded;
    }
    try {
        decoded.decision = Trading::JevHttpClient::parseDecision(response_bytes, state);
    } catch (const std::exception& error) {
        decoded.parse_outcome = "failed";
        decoded.parse_error = error.what();
        return decoded;
    }
    decoded.parse_outcome = "ok";
    return decoded;
}

auto rawReport(const DecodedResponse& decoded) -> Json {
    if (!decoded.raw_answers.is_object()) return nullptr;
    return Json{{"bias", rawAnswerReport(decoded.raw_answers, "bias")},
                                {"intent", rawAnswerReport(decoded.raw_answers, "intent")}};
}

auto postCapturedBytes(const std::string& endpoint, const std::string& api_key, long timeout_ms,
                                             const std::string& bytes, std::string& response,
                                             double& elapsed_ms) -> std::pair<CURLcode, long> {
    static std::once_flag curl_init_once;
    std::call_once(curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    auto* curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");
    auto* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    const auto authorization = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, authorization.c_str());
    const auto title = std::string("X-Title: ") + kReplayHttpTitle;
    headers = curl_slist_append(headers, title.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bytes.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bytes.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const auto perform_started = std::chrono::steady_clock::now();
    const auto result = curl_easy_perform(curl);
    elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                                                                   perform_started)
                             .count();
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return {result, status};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto arguments = parseArguments(argc, argv);
        const auto capture_directory =
                std::filesystem::path(arguments.at("capture").get<std::string>());
        const auto out_root = std::filesystem::path(arguments.at("out").get<std::string>());
        const auto label = arguments.at("label").get<std::string>();
        const auto variant = arguments.at("variant").get<std::string>();
        const bool parse_only = arguments.at("parse_only").get<bool>();

        const auto capture_request_bytes = readRequiredFile(capture_directory / "request.json");
        const auto capture_metadata = readOptionalJson(capture_directory / "metadata.json");
        const auto request_document = Json::parse(capture_request_bytes);
        auto state = evaluationStateFromRequest(request_document);
        const auto request_sha256 = JevHttpCapture::sha256Hex(capture_request_bytes);

        if (parse_only) {
            const auto response_bytes = Trading::readFileBytes(capture_directory / "response.raw");
            const auto decoded = decodeResponse(response_bytes, state);
            std::cout << Json{{"mode", "parse-only"},
                                                {"capture", capture_directory.string()},
                                                {"evaluation_id", state.evaluation_id_},
                                                {"request_sha256", request_sha256},
                                                {"raw", rawReport(decoded)},
                                                {"diagnostics", decoded.raw_answers.is_object()
                                                                                        ? diagnosticChecks(decoded.raw_answers, request_document)
                                                                                        : Json(nullptr)},
                                                {"parsed", decisionReport(decoded.decision)},
                                                {"parse_outcome", decoded.parse_outcome},
                                                {"parse_error", decoded.parse_error.empty()
                                                                                                        ? Json(nullptr)
                                                                                                        : Json(decoded.parse_error)}}
                                    .dump() << '\n';
            return 0;
        }

        const auto* api_key = std::getenv("OPENROUTER_API_KEY");
        if (api_key == nullptr || std::string(api_key).empty()) {
            throw std::runtime_error("OPENROUTER_API_KEY is not set in the environment");
        }

        std::string send_bytes;
        Json variant_diff = Json::array();
        bool state_unchanged = true;
        bool membership_unchanged = true;
        if (variant == "previous") {
            const auto variant_document =
                    Trading::Tools::buildPreviousWordingVariant(request_document);
            variant_diff = Trading::Tools::describeWordingDiff(request_document, variant_document);
            state_unchanged = JevHttpCapture::sha256Hex(request_document.at("state").dump()) ==
                                                JevHttpCapture::sha256Hex(variant_document.at("state").dump());
            membership_unchanged =
                    criteriaKeys(request_document) == criteriaKeys(variant_document);
            send_bytes = variant_document.dump();
        } else {
            send_bytes = capture_request_bytes;
        }

        const auto endpoint = arguments.contains("endpoint")
                                                                        ? arguments.at("endpoint").get<std::string>()
                                                                        : capture_metadata.value("endpoint", "https://openrouter.ai/api/alpha/decisions");
        const auto model = arguments.contains("model")
                                                                       ? arguments.at("model").get<std::string>()
                                                                       : capture_metadata.value("model", "typesafe/jev-1.13");
        const auto timeout_ms = arguments.contains("timeout_ms")
                                                                            ? arguments.at("timeout_ms").get<long>()
                                                                            : capture_metadata.value("timeout_ms", 4000L);

        Trading::JevCaptureAttemptMeta meta;
        meta.evaluation_id_ = state.evaluation_id_;
        meta.ticker_id_ = state.ticker_id_;
        meta.attempt_ = 0;
        meta.endpoint_ = endpoint;
        meta.model_ = model;
        meta.timeout_ms_ = timeout_ms;
        meta.max_retries_ = 0;
        meta.role_ = "replay";
        meta.replay_of_ = capture_metadata.value("run_id", "") + "/" +
                                            capture_directory.filename().string();
        meta.source_request_sha256_ = request_sha256;
        meta.variant_ = variant;
        meta.started_utc_ = JevHttpCapture::utcTimestampNow();

        JevHttpCapture capture({out_root, label});
        const auto context = capture.beginAttempt(meta, send_bytes);
        if (!context.has_value()) {
            throw std::runtime_error("cannot write replay output under " + out_root.string());
        }

        const auto sent_file_bytes = Trading::readFileBytes(context->directory_ / "request.json");
        const auto sent_sha256 = JevHttpCapture::sha256Hex(send_bytes);
        const Json verification = {
                {"sent_bytes_match_request_file", sent_file_bytes == send_bytes},
                {"sent_sha256", sent_sha256},
                {"request_file_sha256", JevHttpCapture::sha256Hex(sent_file_bytes)},
                {"capture_request_sha256", request_sha256},
                {"state_sha256_unchanged", state_unchanged},
                {"criteria_membership_unchanged", membership_unchanged}};
        writeTextFile(context->directory_ / "verification.json", verification.dump() + "\n");
        writeTextFile(context->directory_ / "variant-diff.json", variant_diff.dump() + "\n");

        std::string response;
        double elapsed_ms = 0.0;
        const auto [result, status] = postCapturedBytes(endpoint, api_key, timeout_ms, send_bytes,
                                                                                                        response, elapsed_ms);
        const bool http_ok = result == CURLE_OK && status >= 200 && status < 300;
        auto decoded = http_ok ? decodeResponse(response, state) : DecodedResponse{};
        capture.completeAttempt(context, meta, send_bytes, response, static_cast<int>(result),
                                                        curl_easy_strerror(result),
                                                        result == CURLE_OK
                                                                ? std::optional<long>(status)
                                                                : std::nullopt,
                                                        elapsed_ms, decoded.parse_outcome,
                                                        decoded.parse_error);
        std::cout << Json{{"mode", "replay"},
                                            {"label", label},
                                            {"variant", variant},
                                            {"capture", capture_directory.string()},
                                            {"replay_of", meta.replay_of_},
                                            {"evaluation_id", state.evaluation_id_},
                                            {"request_sha256", sent_sha256},
                                            {"capture_request_sha256", request_sha256},
                                            {"verification", verification},
                                            {"endpoint", endpoint},
                                            {"model", model},
                                            {"curl_outcome", std::string(curl_easy_strerror(result))},
                                            {"http_status", result == CURLE_OK
                                                                                                    ? Json(status)
                                                                                                    : Json(nullptr)},
                                            {"elapsed_ms", elapsed_ms},
                                            {"raw", rawReport(decoded)},
                                            {"diagnostics", decoded.raw_answers.is_object()
                                                                                            ? diagnosticChecks(decoded.raw_answers, request_document)
                                                                                            : Json(nullptr)},
                                            {"parsed", decisionReport(decoded.decision)},
                                            {"parse_outcome", decoded.parse_outcome.empty()
                                                                                                    ? Json(nullptr)
                                                                                                    : Json(decoded.parse_outcome)},
                                            {"parse_error", decoded.parse_error.empty()
                                                                                                    ? Json(nullptr)
                                                                                                    : Json(decoded.parse_error)}}
                                .dump() << '\n';
        return http_ok ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "jev_replay error: " << error.what() << '\n';
        return 1;
    }
}
