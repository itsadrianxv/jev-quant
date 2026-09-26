<!-- Source: https://developers.binance.com/en/docs/llms.txt (section: API Reference > Futures (USDⓈ-M) REST API / WebSocket API / WebSocket Market Streams)
     Fetched: 2026-09-25 -->
### Futures (USDⓈ-M) REST API (1.0.0)

Access market data, manage accounts, and trade USDⓈ-M perpetual futures.

- `GET /fapi/v2/account` — Account Information V2 (USER_DATA)
- `GET /fapi/v3/account` — Account Information V3 (USER_DATA)
- `GET /fapi/v2/balance` — Futures Account Balance V2 (USER_DATA)
- `GET /fapi/v3/balance` — Futures Account Balance V3 (USER_DATA)
- `GET /fapi/v1/accountConfig` — Futures Account Configuration (USER_DATA)
- `GET /fapi/v1/apiTradingStatus` — Futures Trading Quantitative Rules Indicators (USER_DATA)
- `GET /fapi/v1/feeBurn` — Get BNB Burn Status (USER_DATA)
- `POST /fapi/v1/feeBurn` — Toggle BNB Burn On Futures Trade (TRADE)
- `GET /fapi/v1/multiAssetsMargin` — Get Current Multi-Assets Mode (USER_DATA)
- `POST /fapi/v1/multiAssetsMargin` — Change Multi-Assets Mode (TRADE)
- `GET /fapi/v1/positionSide/dual` — Get Current Position Mode (USER_DATA)
- `POST /fapi/v1/positionSide/dual` — Change Position Mode (TRADE)
- `GET /fapi/v1/order/asyn` — Get Download Id For Futures Order History (USER_DATA)
- `GET /fapi/v1/trade/asyn` — Get Download Id For Futures Trade History (USER_DATA)
- `GET /fapi/v1/income/asyn` — Get Download Id For Futures Transaction History (USER_DATA)
- `GET /fapi/v1/order/asyn/id` — Get Futures Order History Download Link by Id (USER_DATA)
- `GET /fapi/v1/trade/asyn/id` — Get Futures Trade Download Link by Id (USER_DATA)
- `GET /fapi/v1/income/asyn/id` — Get Futures Transaction History Download Link by Id (USER_DATA)
- `GET /fapi/v1/income` — Get Income History (USER_DATA)
- `GET /fapi/v1/leverageBracket` — Notional and Leverage Brackets (USER_DATA)
- `GET /fapi/v1/rateLimit/order` — Query User Rate Limit (USER_DATA)
- `GET /fapi/v1/symbolConfig` — Symbol Configuration (USER_DATA)
- `GET /fapi/v1/commissionRate` — User Commission Rate (USER_DATA)
- `POST /fapi/v1/convert/acceptQuote` — Accept the offered quote (USER_DATA)
- `GET /fapi/v1/convert/exchangeInfo` — List All Convert Pairs
- `GET /fapi/v1/convert/orderStatus` — Order status (USER_DATA)
- `POST /fapi/v1/convert/getQuote` — Send Quote Request (USER_DATA)
- `GET /fapi/v1/symbolAdlRisk` — ADL Risk
- `GET /futures/data/basis` — Basis
- `GET /fapi/v1/time` — Check Server Time
- `GET /fapi/v1/indexInfo` — Composite Index Symbol Information
- `GET /fapi/v1/aggTrades` — Compressed/Aggregate Trades List
- `GET /fapi/v1/continuousKlines` — Continuous Contract Kline/Candlestick Data
- `GET /fapi/v1/exchangeInfo` — Exchange Information
- `GET /fapi/v1/fundingRate` — Get Funding Rate History
- `GET /fapi/v1/fundingInfo` — Get Funding Rate Info
- `GET /fapi/v1/indexPriceKlines` — Index Price Kline/Candlestick Data
- `GET /fapi/v1/klines` — Kline/Candlestick Data
- `GET /futures/data/globalLongShortAccountRatio` — Long/Short Ratio
- `GET /fapi/v1/premiumIndex` — Mark Price
- `GET /fapi/v1/markPriceKlines` — Mark Price Kline/Candlestick Data
- `GET /fapi/v1/assetIndex` — Multi-Assets Mode Asset Index
- `GET /fapi/v1/historicalTrades` — Old Trades Lookup (MARKET_DATA)
- `GET /fapi/v1/openInterest` — Open Interest
- `GET /futures/data/openInterestHist` — Open Interest Statistics
- `GET /fapi/v1/depth` — Order Book
- `GET /fapi/v1/premiumIndexKlines` — Premium index Kline Data
- `GET /futures/data/delivery-price` — Quarterly Contract Settlement Price
- `GET /fapi/v1/constituents` — Query Index Price Constituents
- `GET /fapi/v1/insuranceBalance` — Query Insurance Fund Balance Snapshot
- `GET /fapi/v1/trades` — Recent Trades List
- `GET /fapi/v1/rpiDepth` — RPI Order Book
- `GET /fapi/v1/ticker/bookTicker` — Symbol Order Book Ticker
- `GET /fapi/v1/ticker/price` — Symbol Price Ticker
- `GET /fapi/v2/ticker/price` — Symbol Price Ticker V2
- `GET /futures/data/takerlongshortRatio` — Taker Buy/Sell Volume
- `GET /fapi/v1/ping` — Test Connectivity
- `GET /fapi/v1/ticker/24hr` — 24hr Ticker Price Change Statistics
- `GET /futures/data/topLongShortAccountRatio` — Top Trader Long/Short Account Ratio (MARKET_DATA)
- `GET /futures/data/topLongShortPositionRatio` — Top Trader Long/Short Position Ratio (MARKET_DATA)
- `GET /fapi/v1/tradingSchedule` — Trading Schedule
- `GET /fapi/v1/pmAccountInfo` — Classic Portfolio Margin Account Information (USER_DATA)
- `GET /fapi/v1/userTrades` — Account Trade List (USER_DATA)
- `GET /fapi/v1/allOrders` — All Orders (USER_DATA)
- `POST /fapi/v1/countdownCancelAll` — Auto-Cancel All Open Orders (TRADE)
- `GET /fapi/v1/algoOrder` — Query Algo Order (USER_DATA)
- `POST /fapi/v1/algoOrder` — New Algo Order (TRADE)
- `DELETE /fapi/v1/algoOrder` — Cancel Algo Order (TRADE)
- `DELETE /fapi/v1/algoOpenOrders` — Cancel All Algo Open Orders (TRADE)
- `DELETE /fapi/v1/allOpenOrders` — Cancel All Open Orders (TRADE)
- `PUT /fapi/v1/batchOrders` — Modify Multiple Orders (TRADE)
- `POST /fapi/v1/batchOrders` — Place Multiple Orders (TRADE)
- `DELETE /fapi/v1/batchOrders` — Cancel Multiple Orders (TRADE)
- `GET /fapi/v1/order` — Query Order (USER_DATA)
- `PUT /fapi/v1/order` — Modify Order (TRADE)
- `POST /fapi/v1/order` — New Order (TRADE)
- `DELETE /fapi/v1/order` — Cancel Order (TRADE)
- `POST /fapi/v1/leverage` — Change Initial Leverage (TRADE)
- `POST /fapi/v1/marginType` — Change Margin Type (TRADE)
- `GET /fapi/v1/openAlgoOrders` — Current All Algo Open Orders (USER_DATA)
- `GET /fapi/v1/openOrders` — Current All Open Orders (USER_DATA)
- `POST /fapi/v1/stock/contract` — Futures TradFi Perps Contract (USER_DATA)
- `GET /fapi/v1/orderAmendment` — Get Order Modify History (USER_DATA)
- `GET /fapi/v1/positionMargin/history` — Get Position Margin Change History (TRADE)
- `POST /fapi/v1/positionMargin` — Modify Isolated Position Margin (TRADE)
- `GET /fapi/v1/adlQuantile` — Position ADL Quantile Estimation (USER_DATA)
- `GET /fapi/v2/positionRisk` — Position Information V2 (USER_DATA)
- `GET /fapi/v3/positionRisk` — Position Information V3 (USER_DATA)
- `GET /fapi/v1/allAlgoOrders` — Query All Algo Orders (USER_DATA)
- `GET /fapi/v1/openOrder` — Query Current Open Order (USER_DATA)
- `POST /fapi/v1/order/test` — Test Order (TRADE)
- `GET /fapi/v1/forceOrders` — User's Force Orders (USER_DATA)
- `PUT /fapi/v1/listenKey` — Keepalive User Data Stream (USER_STREAM)
- `POST /fapi/v1/listenKey` — Start User Data Stream (USER_STREAM)
- `DELETE /fapi/v1/listenKey` — Close User Data Stream (USER_STREAM)

### Futures (USDⓈ-M) WebSocket API (1.0.0)

Access market data, manage accounts, and trade USDⓈ-M perpetual futures.

- `POST /account.status` — Account Information (USER_DATA)
- `POST /v2/account.status` — Account Information V2 (USER_DATA)
- `POST /account.balance` — Futures Account Balance (USER_DATA)
- `POST /v2/account.balance` — Futures Account Balance V2 (USER_DATA)
- `POST /depth` — Order Book
- `POST /ticker.book` — Symbol Order Book Ticker
- `POST /ticker.price` — Symbol Price Ticker
- `POST /algoOrder.cancel` — Cancel Algo Order (TRADE)
- `POST /order.cancel` — Cancel Order (TRADE)
- `POST /order.modify` — Modify Order (TRADE)
- `POST /algoOrder.place` — New Algo Order (TRADE)
- `POST /order.place` — New Order (TRADE)
- `POST /account.position` — Position Information (USER_DATA)
- `POST /v2/account.position` — Position Information V2 (USER_DATA)
- `POST /order.status` — Query Order (USER_DATA)
- `POST /userDataStream.stop` — Close User Data Stream (USER_STREAM)
- `POST /userDataStream.ping` — Keepalive User Data Stream (USER_STREAM)
- `POST /userDataStream.start` — Start User Data Stream (USER_STREAM)

### Futures (USDⓈ-M) WebSocket Market Streams (1.0.0)

Access market data, manage accounts, and trade USDⓈ-M perpetual futures.

- `POST /{symbol}@aggTrade` — Aggregate Trade Streams
- `POST /!forceOrder@arr` — All Market Liquidation Order Streams
- `POST /!miniTicker@arr` — All Market Mini Tickers Stream
- `POST /!ticker@arr` — All Market Tickers Streams
- `POST /{symbol}@compositeIndex` — Composite Index Symbol Information Streams
- `POST /{pair}_{contractType}@continuousKline_{interval}` — Continuous Contract Kline/Candlestick Streams
- `POST /!contractInfo` — Contract Info Stream
- `POST /{symbol}@miniTicker` — Individual Symbol Mini Ticker Stream
- `POST /{symbol}@ticker` — Individual Symbol Ticker Streams
- `POST /{symbol}@kline_{interval}` — Kline/Candlestick Streams
- `POST /{symbol}@forceOrder` — Liquidation Order Streams
- `POST /{symbol}@markPrice@{updateSpeed}` — Mark Price Stream
- `POST /!markPrice@arr@{updateSpeed}` — Mark Price Stream for All market
- `POST /!assetIndex@arr` — Multi-Assets Mode Asset Index
- `POST /tradingSession` — Trading Session Stream
- `POST /!bookTicker` — All Book Tickers Stream
- `POST /{symbol}@depth@{updateSpeed}` — Diff. Book Depth Streams
- `POST /{symbol}@bookTicker` — Individual Symbol Book Ticker Streams
- `POST /{symbol}@depth{levels}@{updateSpeed}` — Partial Book Depth Streams
- `POST /{symbol}@rpiDepth@500ms` — RPI Diff. Book Depth Streams
