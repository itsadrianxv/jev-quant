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
    src/trading/strategy/order_manager.cpp
    src/trading/strategy/trade_engine.cpp
)
target_link_libraries(jev_trading PUBLIC jev_domain)
target_include_directories(jev_trading PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(jev_trading PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()

add_executable(contract_tests tests/contract_tests.cpp)
target_link_libraries(contract_tests PRIVATE jev_trading)
target_compile_options(contract_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_test(NAME contract_tests COMMAND contract_tests)
