cmake_minimum_required(VERSION 3.20)

project(jev_quant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(jev_common INTERFACE)
target_include_directories(jev_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(jev_domain INTERFACE)
target_link_libraries(jev_domain INTERFACE jev_common)

add_library(jev_trading STATIC
    src/trading/market_data/market_data_consumer.cpp
    src/trading/order_gw/order_gateway.cpp
    src/trading/strategy/jev_worker.cpp
    src/trading/strategy/jev_http_client.cpp
    src/trading/strategy/order_manager.cpp
    src/trading/strategy/trade_engine.cpp
)
target_link_libraries(jev_trading PUBLIC jev_domain)
target_include_directories(jev_trading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(jev_trading PRIVATE -Wall -Wextra -Wpedantic -Werror)

set(JEV_QUANT_CURL_ROOT "" CACHE PATH "Root containing libcurl development headers and libraries")
find_path(JEV_QUANT_CURL_INCLUDE_DIR curl/curl.h
    HINTS
      "${JEV_QUANT_CURL_ROOT}/include"
      "${JEV_QUANT_CURL_ROOT}/usr/include"
      "${JEV_QUANT_CURL_ROOT}/usr/include/x86_64-linux-gnu")
find_file(JEV_QUANT_CURL_SHARED_LIBRARY
    NAMES libcurl.so.4 libcurl.so
    HINTS
      "/lib/x86_64-linux-gnu"
      "/usr/lib/x86_64-linux-gnu"
      "${JEV_QUANT_CURL_ROOT}/lib"
      "${JEV_QUANT_CURL_ROOT}/usr/lib"
      "${JEV_QUANT_CURL_ROOT}/usr/lib/x86_64-linux-gnu")
if (JEV_QUANT_CURL_SHARED_LIBRARY)
  set(JEV_QUANT_CURL_LIBRARY "${JEV_QUANT_CURL_SHARED_LIBRARY}")
else()
  find_library(JEV_QUANT_CURL_LIBRARY NAMES curl
      HINTS
        "${JEV_QUANT_CURL_ROOT}/lib"
        "${JEV_QUANT_CURL_ROOT}/usr/lib"
        "${JEV_QUANT_CURL_ROOT}/usr/lib/x86_64-linux-gnu")
endif()
if (JEV_QUANT_CURL_INCLUDE_DIR AND JEV_QUANT_CURL_LIBRARY)
  target_compile_definitions(jev_trading PUBLIC JEV_QUANT_HAS_CURL=1)
  target_include_directories(jev_trading PUBLIC "${JEV_QUANT_CURL_INCLUDE_DIR}")
  target_link_libraries(jev_trading PUBLIC "${JEV_QUANT_CURL_LIBRARY}")
else()
  target_compile_definitions(jev_trading PUBLIC JEV_QUANT_HAS_CURL=0)
endif()

enable_testing()

add_executable(contract_tests tests/contract_tests.cpp)
target_link_libraries(contract_tests PRIVATE jev_trading)
target_compile_options(contract_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME contract_tests COMMAND contract_tests)
