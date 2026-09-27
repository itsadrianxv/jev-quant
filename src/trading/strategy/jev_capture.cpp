#include "jev_capture.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <openssl/evp.h>

#ifndef JEV_SOURCE_REVISION
#define JEV_SOURCE_REVISION "unknown"
#endif

namespace Trading {
namespace {

auto writeBytes(const std::filesystem::path& path, std::string_view bytes) -> void {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot open " + path.string());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("cannot write " + path.string());
}

}  // namespace

auto readFileBytes(const std::filesystem::path& path) -> std::string {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

auto JevHttpCapture::utcTimestampNow() -> std::string {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()) % std::chrono::milliseconds(1000);
    std::tm broken{};
#ifdef _WIN32
    gmtime_s(&broken, &seconds);
#else
    gmtime_r(&seconds, &broken);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &broken);
    char stamped[40];
    std::snprintf(stamped, sizeof(stamped), "%s.%03dZ", buffer,
                  static_cast<int>(millis.count()));
    return stamped;
}

auto JevHttpCapture::sha256Hex(std::string_view bytes) -> std::string {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1 ||
        length != 32) {
        return {};
    }
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(length * 2);
    for (unsigned int index = 0; index < length; ++index) {
        hex.push_back(kHexDigits[digest[index] >> 4]);
        hex.push_back(kHexDigits[digest[index] & 0x0F]);
    }
    return hex;
}

auto JevHttpCapture::runningBinarySha256() -> std::string {
#if defined(__linux__)
    const auto bytes = readFileBytes("/proc/self/exe");
    if (!bytes.empty()) return sha256Hex(bytes);
#endif
    return "unavailable";
}

auto JevHttpCapture::beginAttempt(const JevCaptureAttemptMeta& meta,
                                                                const std::string& request_body) const
        -> std::optional<JevCaptureAttemptContext> {
    if (!enabled()) return std::nullopt;
    JevCaptureAttemptContext context;
    context.sequence_ = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    context.directory_ = std::filesystem::path(config_.directory_) / config_.run_id_ /
                                             (std::to_string(context.sequence_) + "-eval" +
                                              std::to_string(meta.evaluation_id_) + "-attempt" +
                                              std::to_string(meta.attempt_));
    try {
        std::filesystem::create_directories(context.directory_);
        writeBytes(context.directory_ / "request.json", request_body);
    } catch (const std::exception& error) {
        std::cerr << "event=jev_capture_write_failed stage=begin_attempt error=" << error.what()
                  << '\n';
        return std::nullopt;
    }
    return context;
}

auto JevHttpCapture::completeAttempt(
        const std::optional<JevCaptureAttemptContext>& context, const JevCaptureAttemptMeta& meta,
        const std::string& request_body, const std::string& response_body, int curl_code,
        std::string_view curl_outcome, std::optional<long> http_status, double elapsed_ms,
        std::string_view parse_outcome, std::string_view parse_error) const -> void {
    if (!context.has_value()) return;
    const nlohmann::json request_sha256 = sha256Hex(request_body);
    const nlohmann::json response_sha256 = sha256Hex(response_body);
    const nlohmann::json metadata = {
            {"run_id", config_.run_id_},
            {"sequence", context->sequence_},
            {"role", meta.role_},
            {"replay_of", meta.replay_of_},
            {"source_request_sha256", meta.source_request_sha256_},
            {"variant", meta.variant_},
            {"evaluation_id", meta.evaluation_id_},
            {"ticker_id", meta.ticker_id_},
            {"attempt", meta.attempt_},
            {"started_utc", meta.started_utc_},
            {"endpoint", meta.endpoint_},
            {"model", meta.model_},
            {"timeout_ms", meta.timeout_ms_},
            {"max_retries", meta.max_retries_},
            {"request_sha256", request_sha256},
            {"request_bytes", request_body.size()},
            {"response_sha256", response_sha256},
            {"response_bytes", response_body.size()},
            {"curl_code", curl_code},
            {"curl_outcome", std::string(curl_outcome)},
            {"http_status", http_status.has_value()
                                                            ? nlohmann::json(*http_status)
                                                            : nlohmann::json(nullptr)},
            {"elapsed_ms", elapsed_ms},
            {"parse_outcome", parse_outcome.empty() ? nlohmann::json(nullptr)
                                                                                              : nlohmann::json(std::string(parse_outcome))},
            {"parse_error", parse_error.empty() ? nlohmann::json(nullptr)
                                                                          : nlohmann::json(std::string(parse_error))},
            {"source_revision", JEV_SOURCE_REVISION},
            {"binary_sha256", runningBinarySha256()}};
    try {
        writeBytes(context->directory_ / "response.raw", response_body);
        writeBytes(context->directory_ / "metadata.json", metadata.dump() + "\n");
    } catch (const std::exception& error) {
        std::cerr << "event=jev_capture_write_failed stage=complete_attempt error=" << error.what()
                  << '\n';
    }
}

}  // namespace Trading
