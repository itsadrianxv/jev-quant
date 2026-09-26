cmake_minimum_required(VERSION 3.24)

project(jev_quant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(jev_common INTERFACE)
target_include_directories(jev_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(jev_domain INTERFACE)
target_link_libraries(jev_domain INTERFACE jev_common)

add_library(jev_trading_core STATIC
        src/trading/market_data/market_data_consumer.cpp
        src/trading/order_gw/order_gateway.cpp
        src/trading/strategy/jev_worker.cpp
        src/trading/strategy/jev_http_client.cpp
        src/trading/strategy/jev_config.cpp
        src/trading/strategy/order_manager.cpp
        src/trading/strategy/trade_engine.cpp
        src/trading/venue/venue_adapter.cpp
        src/trading/venue/binance_websocket_stream.cpp
)
target_link_libraries(jev_trading_core PUBLIC jev_domain)
target_include_directories(jev_trading_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(jev_trading_core PRIVATE -Wall -Wextra -Wpedantic)

option(JEV_ENABLE_SIMEX "Build the localhost simex venue adapter" ON)
set(JEV_SIMEX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../simex" CACHE PATH "Pinned simex source checkout")
if(JEV_ENABLE_SIMEX)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The simex v1 integration requires 64-bit little-endian Linux/WSL")
    endif()
    include(cmake/SimexProtocol.cmake)
    target_sources(jev_trading_core PRIVATE src/trading/venue/simex_venue_adapter.cpp)
    target_include_directories(jev_trading_core PRIVATE "${JEV_SIMEX_SOURCE_DIR}/src")
    target_compile_definitions(jev_trading_core PUBLIC JEV_ENABLE_SIMEX=1)
endif()

# Prefer a system-provided libcurl.  Source fallback is opt-in so normal builds
# remain fast and deterministic.
option(JEV_FETCH_CURL "Build curl from source when no system package exists" OFF)
option(JEV_FETCH_NLOHMANN_JSON "Fetch nlohmann/json when no system package exists" OFF)
option(JEV_ENABLE_LIVE_TESTS "Build tests that call the configured OpenRouter API" OFF)
find_package(nlohmann_json CONFIG QUIET)
if (NOT nlohmann_json_FOUND AND JEV_FETCH_NLOHMANN_JSON)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()
if (NOT nlohmann_json_FOUND AND NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)
target_link_libraries(jev_trading_core PUBLIC Boost::headers)
find_package(CURL QUIET)
if (NOT CURL_FOUND AND JEV_FETCH_CURL)
    include(FetchContent)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    # Prefer OpenSSL when its development package is available.  The fallback
    # remains buildable on a minimal Linux image by disabling TLS backends only
    # when no TLS development package can be found.
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_GNUTLS OFF CACHE BOOL "" FORCE)
        set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
        set(CURL_USE_WOLFSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_BEARSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_RUSTLS OFF CACHE BOOL "" FORCE)
    endif()
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)

    FetchContent_Declare(curl
            URL https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.xz
            FIND_PACKAGE_ARGS)
    FetchContent_MakeAvailable(curl)
endif()

if (NOT CURL_FOUND AND NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
endif()

if (NOT TARGET CURL::libcurl)
    message(FATAL_ERROR "libcurl was found but did not provide CURL::libcurl")
endif()
target_link_libraries(jev_trading_core PUBLIC CURL::libcurl OpenSSL::Crypto OpenSSL::SSL)
target_link_libraries(jev_trading_core PUBLIC nlohmann_json::nlohmann_json)

enable_testing()

add_executable(contract_tests tests/contract_tests.cpp)
target_link_libraries(contract_tests PRIVATE jev_trading_core)
target_compile_options(contract_tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME contract_tests COMMAND contract_tests)

add_executable(jev_trading src/trading_main.cpp)
target_link_libraries(jev_trading PRIVATE jev_trading_core)
target_compile_options(jev_trading PRIVATE -Wall -Wextra -Wpedantic)

if (JEV_ENABLE_LIVE_TESTS)
    add_executable(jev_live_smoke tests/jev_live_smoke.cpp)
    add_executable(binance_um_futures_smoke tests/binance_um_futures_smoke.cpp)
    target_link_libraries(binance_um_futures_smoke PRIVATE jev_trading_core)
    target_compile_options(binance_um_futures_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(jev_live_smoke PRIVATE jev_trading_core)
    target_compile_options(jev_live_smoke PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME jev_live_smoke COMMAND jev_live_smoke)
    set_tests_properties(jev_live_smoke PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
