#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Trading {

/// Opt-in diagnostic capture configuration.  An empty directory disables
/// capture; no credential material may ever be written through this path.
struct JevCaptureConfig {
    std::string directory_;
    std::string run_id_;
};

/// Caller-supplied identity of one HTTP attempt crossing the transport boundary.
struct JevCaptureAttemptMeta {
    std::uint64_t evaluation_id_ = 0;
    std::uint64_t ticker_id_ = 0;
    unsigned attempt_ = 0;
    std::string endpoint_;
    std::string model_;
    long timeout_ms_ = 0;
    unsigned max_retries_ = 0;
    /// "live" for trading-process captures, "replay" for offline replays.
    std::string role_ = "live";
    /// Capture identifier "<run_id>/<attempt-dir>" when replaying a capture.
    std::string replay_of_;
    /// SHA-256 of the originally captured request bytes (replays only).
    std::string source_request_sha256_;
    /// "current" or "previous" for replays; empty for live captures.
    std::string variant_;
    /// UTC timestamp taken when the HTTP attempt started.
    std::string started_utc_;
};

/// Directory and global sequence number assigned to one recorded attempt.
struct JevCaptureAttemptContext {
    std::filesystem::path directory_;
    std::uint64_t sequence_ = 0;
};

/// Writes the exact bytes crossing the Jev HTTP transport boundary.  Each HTTP
/// attempt gets its own directory holding request.json, response.raw, and
/// metadata.json; asynchronous captures therefore cannot overwrite or mispair
/// records.  Capture write failures are reported on stderr and never alter the
/// trading behavior under diagnosis.
class JevHttpCapture {
  public:
    JevHttpCapture() = default;
    explicit JevHttpCapture(JevCaptureConfig config) : config_(std::move(config)) {}

    [[nodiscard]] auto enabled() const -> bool { return !config_.directory_.empty(); }
    [[nodiscard]] auto runId() const -> const std::string& { return config_.run_id_; }
    auto setRunId(std::string run_id) -> void { config_.run_id_ = std::move(run_id); }

    /// Creates the attempt directory and writes request.json with the exact
    /// request bytes before the HTTP call is performed.  Returns nullopt when
    /// capture is disabled or the directory cannot be written.
    auto beginAttempt(const JevCaptureAttemptMeta& meta, const std::string& request_body) const
            -> std::optional<JevCaptureAttemptContext>;

    /// Writes response.raw (the untouched response bytes, possibly partial or
    /// empty) and metadata.json for the attempt directory.
    auto completeAttempt(const std::optional<JevCaptureAttemptContext>& context,
                         const JevCaptureAttemptMeta& meta, const std::string& request_body,
                         const std::string& response_body, int curl_code,
                         std::string_view curl_outcome, std::optional<long> http_status,
                         double elapsed_ms, std::string_view parse_outcome,
                         std::string_view parse_error) const -> void;

    /// Current UTC time as e.g. 2026-09-27T12:34:56.789Z.
    [[nodiscard]] static auto utcTimestampNow() -> std::string;

    /// Lowercase hexadecimal SHA-256 of the given bytes.
    [[nodiscard]] static auto sha256Hex(std::string_view bytes) -> std::string;

    /// SHA-256 of the running executable image, or "unavailable".
    [[nodiscard]] static auto runningBinarySha256() -> std::string;

  private:
    JevCaptureConfig config_;
    mutable std::atomic<std::uint64_t> sequence_{0};
};

/// Reads a whole file as bytes; returns an empty string when it cannot be read.
[[nodiscard]] auto readFileBytes(const std::filesystem::path& path) -> std::string;

}  // namespace Trading
