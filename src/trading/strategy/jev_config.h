#pragma once

#include <string>

#include "jev_http_client.h"

namespace Trading {

/// Loads the non-sensitive Jev HTTP settings and the API key from local files.
auto loadJevHttpConfig(const std::string& dotenv_path = ".env",
                                              const std::string& json_path = "config.json")
        -> JevHttpConfig;

}  // namespace Trading
