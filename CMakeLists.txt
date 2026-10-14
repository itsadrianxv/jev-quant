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
        src/trading/strategy/jev_capture.cpp
        src/trading/strategy/jev_http_client.cpp
        src/trading/strategy/jev_config.cpp
        src/trading/strategy/order_manager.cpp
        src/trading/strategy/trade_engine.cpp
        src/trading/venue/venue_adapter.cpp
        src/trading/venue/binance_depth_book.cpp
        src/trading/venue/binance_websocket_stream.cpp
        src/tools/jev_variant.cpp
)
target_link_libraries(jev_trading_core PUBLIC jev_domain)
target_include_directories(jev_trading_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(jev_trading_core PRIVATE -Wall -Wextra -Wpedantic)

option(JEV_ENABLE_SIMEX "Build the localhost simex venue adapter" ON)
option(JEV_ENABLE_SIMNOW "Build the Linux x64 SimNow CTP adapter" ON)
if(JEV_ENABLE_SIMNOW)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
       NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        message(FATAL_ERROR "SimNow requires Linux x64; disable JEV_ENABLE_SIMNOW on other platforms")
    endif()
    set(CTP_SDK "${CMAKE_CURRENT_SOURCE_DIR}/docs/simnow/sdk")
    foreach(component IN ITEMS trader md)
        add_library(ctp_${component} SHARED IMPORTED)
    endforeach()
    set_target_properties(ctp_trader PROPERTIES IMPORTED_LOCATION "${CTP_SDK}/lib/linux64/thosttraderapi_se.so")
    set_target_properties(ctp_md PROPERTIES IMPORTED_LOCATION "${CTP_SDK}/lib/linux64/thostmduserapi_se.so")
    target_sources(jev_trading_core PRIVATE src/trading/venue/simnow_venue_adapter.cpp src/trading/venue/ctp_transport.cpp)
    target_include_directories(jev_trading_core SYSTEM PUBLIC "${CTP_SDK}/headers")
    target_link_libraries(jev_trading_core PUBLIC ctp_trader ctp_md)
    target_compile_definitions(jev_trading_core PUBLIC JEV_ENABLE_SIMNOW=1)
endif()
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

# Source revision recorded by the opt-in diagnostic capture.  Build copies may
# have no .git directory, so an explicit cache override is supported.
set(JEV_SOURCE_REVISION "" CACHE STRING "Source revision recorded by diagnostic capture")
if(NOT JEV_SOURCE_REVISION)
    execute_process(COMMAND git rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE JEV_GIT_REVISION
            ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(JEV_GIT_REVISION STREQUAL "")
        set(JEV_SOURCE_REVISION "unknown")
    endif()
endif()
target_compile_definitions(jev_trading_core PRIVATE JEV_SOURCE_REVISION="${JEV_SOURCE_REVISION}")

enable_testing()

# GoogleTest backs the layered test tree (unit, component, integration). The
# release is pinned so builds stay reproducible without a system googletest
# package.
include(FetchContent)
set(JEV_GTEST_VERSION "v1.15.2" CACHE STRING "Pinned GoogleTest release for the layered test tree")
FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/${JEV_GTEST_VERSION}.tar.gz)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# Registers a GoogleTest binary whose cases become individual CTest entries,
# each tagged with the given layer label (unit, component, or integration).
function(jev_add_gtest_test name)
    cmake_parse_arguments(ARG "" "LABEL" "SOURCES" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE jev_trading_core GTest::gtest_main)
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    gtest_discover_tests(${name} PROPERTIES LABELS ${ARG_LABEL}
            DISCOVERY_TIMEOUT 120)
endfunction()

# Offline component and synchronous pipeline measurements; no network.
add_executable(jev_bench src/tools/jev_bench.cpp)
target_link_libraries(jev_bench PRIVATE jev_trading_core)
target_compile_options(jev_bench PRIVATE -Wall -Wextra -Wpedantic)

# The percentile analysis script is standard-library-only; verify it against
# the committed fixture whenever an interpreter is available.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_test(NAME benchmark_contract
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/benchmark_test.py"
                    $<TARGET_FILE:jev_bench>)
    set_tests_properties(benchmark_contract PROPERTIES LABELS integration TIMEOUT 180)
    add_test(NAME ttt_percentiles_fixture
                    COMMAND ${Python3_EXECUTABLE}
                            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ttt_percentiles.py"
                            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/testdata/ttt-fixture.log"
                            --check "${CMAKE_CURRENT_SOURCE_DIR}/scripts/testdata/ttt-fixture-expected.tsv")
endif()

if(JEV_ENABLE_SIMNOW)
    jev_add_gtest_test(simnow_adapter_tests LABEL integration
            SOURCES tests/integration/simnow_adapter_test.cpp)
    add_executable(simnow_live_smoke tests/integration/simnow_live_smoke.cpp)
    target_link_libraries(simnow_live_smoke PRIVATE jev_trading_core)
endif()

if(JEV_ENABLE_SIMEX)
    jev_add_gtest_test(simex_adapter_tests LABEL integration
            SOURCES tests/integration/simex_adapter_test.cpp)
    target_include_directories(simex_adapter_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src" "${JEV_SIMEX_SOURCE_DIR}/src")
endif()

jev_add_gtest_test(unit_tests LABEL unit
        SOURCES tests/unit/position_keeper_test.cpp
                tests/unit/risk_manager_test.cpp
tests/unit/lf_queue_test.cpp
                tests/unit/message_builder_test.cpp
                tests/unit/async_logger_test.cpp
                tests/unit/mem_pool_test.cpp
                tests/unit/market_order_book_test.cpp
                tests/unit/binance_depth_book_test.cpp
                tests/unit/feature_engine_test.cpp
                tests/unit/jev_decision_queue_test.cpp
                tests/unit/jev_decision_request_test.cpp
                tests/unit/jev_decision_parser_test.cpp
                tests/unit/jev_variant_test.cpp
                tests/unit/binance_um_futures_adapter_test.cpp
                tests/unit/message_contract_test.cpp)
jev_add_gtest_test(component_tests LABEL component
        SOURCES tests/component/component_tests.cpp
                tests/component/decision_matrix_test.cpp
                tests/component/order_state_machine_test.cpp
                tests/component/processing_observer_test.cpp)
jev_add_gtest_test(integration_tests LABEL integration
        SOURCES tests/integration/integration_tests.cpp
                tests/integration/market_data_worker_test.cpp
                tests/integration/async_logger_test.cpp)
# The integration suite locates the local mock decision server by absolute
# source path because ctest invokes each case from the build directory.
target_compile_definitions(integration_tests PRIVATE
        JEV_TEST_MOCK_SERVER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mock_jev_server.py")

add_executable(jev_replay src/tools/jev_replay.cpp)
target_link_libraries(jev_replay PRIVATE jev_trading_core)
target_compile_options(jev_replay PRIVATE -Wall -Wextra -Wpedantic)

add_executable(jev_trading src/trading_main.cpp)
target_link_libraries(jev_trading PRIVATE jev_trading_core)
target_compile_options(jev_trading PRIVATE -Wall -Wextra -Wpedantic)

if (JEV_ENABLE_LIVE_TESTS)
    add_executable(jev_live_smoke tests/integration/jev_live_smoke.cpp)
    add_executable(binance_um_futures_smoke tests/integration/binance_um_futures_smoke.cpp)
    target_link_libraries(binance_um_futures_smoke PRIVATE jev_trading_core)
    target_compile_options(binance_um_futures_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(jev_live_smoke PRIVATE jev_trading_core)
    target_compile_options(jev_live_smoke PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME jev_live_smoke COMMAND jev_live_smoke)
    set_tests_properties(jev_live_smoke PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
