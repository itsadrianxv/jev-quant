#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "trading/market_data/market_data_consumer.h"
#include "trading/order_gw/order_gateway.h"
#include "trading/venue/venue_adapter.h"

int main() {
    if (std::getenv("BINANCE_UM_FUTURES_SMOKE") == nullptr) {
        std::cout << "set BINANCE_UM_FUTURES_SMOKE=1 to run Binance demo smoke test\n";
        return 0;
    }
    Exchange::MarketUpdateLFQueue market_updates(1024);
    Exchange::ClientRequestLFQueue requests(1024);
    Exchange::ClientResponseLFQueue responses(1024);
    Trading::MarketDataConsumer market_data(1, &market_updates);
    Trading::OrderGateway gateway(1, &requests, &responses);
    auto config = Trading::BinanceUmFuturesVenueAdapter::loadConfigFromEnv(".env", 0);
    Trading::BinanceUmFuturesVenueAdapter adapter(&market_data, &gateway, std::move(config));
    adapter.start();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    adapter.stop();
    std::cout << "Binance UM Futures connectivity smoke passed\n";
    return 0;
}

