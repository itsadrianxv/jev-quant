<!-- Source: http://openctp.cn/Trading.html
     Fetched: 2026-09-26 -->

# openctp simulation environment (7x24 CTPAPI alternative to SimNow)

openctp operates three CTPAPI-compatible simulation environments. They are a
practical alternative to SimNow when the SimNow fronts are congested (a
common problem) or when a 7x24 environment is needed outside trading hours.

## Environments

- **7x24 environment**: replays the most recent trading day's tick data
  around the clock with simulated matching; only settles briefly at night.
  Good for development and debugging.
- **仿真环境**: follows real trading sessions and real market data; usable
  for strategy validation.
- **vip仿真环境**: paid dedicated-host simulation for professional users.

## Front addresses (openctp TTS, BrokerID 9999, no AppID/AuthCode)

| Environment | Trading front | Market-data front |
|-------------|---------------|-------------------|
| 7x24 | `tcp://trading.openctp.cn:30001` | `tcp://trading.openctp.cn:30011` |
| 仿真 | `tcp://trading.openctp.cn:30002` | Direct connection to a real production market-data front |
| vip仿真 | `tcp://vip.openctp.cn:30003` | Direct connection to a real production market-data front |

Important: connecting requires the TTS CTPAPI-compatible dll/so, **not** the
official CTP libraries — the official libraries fail with error 4097
against TTS fronts.

## Matching rules

- Instruments `TEST`, `BTC`, `MINUS` match user-to-user with
  price-time-priority; everything else is market-maker mode.
- Market-maker mode fills against the live order book: buy orders above the
  best ask and sell orders below the best bid fill immediately, others rest.
  SimNow uses the same market-maker mode.
- Partial fills are only testable in 7x24 mode on `TEST`/`BTC`/`MINUS`;
  the market-maker mode does not produce partial fills.

## Account registration and management

Accounts are issued through the openctp WeChat official account (QR code on
the page): following it grants one 7x24 and one 仿真 account; replying
"注册24" / "注册仿真" issues more. Deposits/withdrawals and account resets
are also performed via message commands (documented on the page).

## Related openctp services

- 数据中心 (Data Center): RESTful JSON market reference data for all China
  futures/options/stocks instruments, no registration required
  (<http://openctp.cn/DataCenter.html>)
- TickTrader: openctp trading client supporting CTP and TTS counters
