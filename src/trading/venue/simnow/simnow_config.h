#pragma once
#include <chrono>
#include <cstddef>
#include <string>
#include <nlohmann/json_fwd.hpp>
#include "common/types.h"

namespace Trading {
class Measurement;
struct SimNowConfig {
    Measurement* measurement = nullptr;
    std::string trading_front, market_front, instrument;
    std::string broker = "9999", app_id = "simnow_client_test", auth_code;
    std::string user, password, flow_directory = "flows/simnow";
    Common::TickerId ticker_id = 0;
    std::size_t capacity = 4096;
    std::chrono::milliseconds startup_timeout{60000}, shutdown_timeout{10000};
    std::chrono::milliseconds query_interval{1000};
    static SimNowConfig fromJson(const nlohmann::json&, Common::TickerId,
                                const std::string& dotenv_path = ".env");
};

}  // namespace Trading
