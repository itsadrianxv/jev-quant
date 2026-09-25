#include "jev_config.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace Trading {
namespace {

auto trim(std::string value) -> std::string {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

auto dotenvValue(const std::string& path) -> std::string {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot open dotenv file: " + path);

    std::string line;
    while (std::getline(file, line)) {
        line = trim(std::move(line));
        if (line.empty() || line.front() == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            throw std::runtime_error("Malformed dotenv line");
        }
        const auto key = trim(line.substr(0, equals));
        auto value = trim(line.substr(equals + 1));
        if (key.empty()) throw std::runtime_error("Empty dotenv key");
        if (value.size() >= 2 &&
                ((value.front() == '"' && value.back() == '"') ||
                  (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        if (key == "OPENROUTER_API_KEY") return value;
    }
    throw std::runtime_error("OPENROUTER_API_KEY is missing from " + path);
}

}  // namespace

auto loadJevHttpConfig(const std::string& dotenv_path,
                                              const std::string& json_path) -> JevHttpConfig {
    JevHttpConfig config;
    config.api_key_ = dotenvValue(dotenv_path);

    std::ifstream file(json_path);
    if (!file) throw std::runtime_error("Cannot open config file: " + json_path);
    nlohmann::json document;
    file >> document;
    const auto settings = document.value("jev", nlohmann::json::object());
    if (!settings.is_object()) throw std::runtime_error("config.jev must be an object");

    if (settings.contains("endpoint")) config.endpoint_ = settings.at("endpoint").get<std::string>();
    if (settings.contains("model")) config.model_ = settings.at("model").get<std::string>();
    if (settings.contains("timeout_ms")) config.timeout_ms_ = settings.at("timeout_ms").get<long>();
    if (settings.contains("max_retries")) config.max_retries_ = settings.at("max_retries").get<unsigned>();
    if (settings.contains("http_referer")) config.http_referer_ = settings.at("http_referer").get<std::string>();
    if (settings.contains("http_title")) config.http_title_ = settings.at("http_title").get<std::string>();
    if (config.endpoint_.empty() || config.model_.empty() || config.timeout_ms_ <= 0) {
        throw std::runtime_error("Invalid Jev HTTP configuration");
    }
    return config;
}

}  // namespace Trading
