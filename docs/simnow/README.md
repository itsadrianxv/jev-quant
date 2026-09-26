<!-- Source: curated from http://www.openctp.cn/CTPAPI.html and http://openctp.cn/Trading.html
     Fetched: 2026-09-26 -->

# SimNow (CTP) venue reference documentation

Reference material for connecting the trading system to SimNow, the CTP
futures simulation venue operated by 上期技术 (SHFE Technology). CTP is the
dominant China futures trading counter, and SimNow is its official public
simulation environment. CTPAPI is a native C++ callback API over TCP (not
HTTP/WebSocket), so a SimNow venue adapter differs fundamentally from the
Binance UM futures demo adapter.

## SimNow environments and front addresses

All environments use BrokerID `9999`, AppID `simnow_client_test`, and
AuthCode `0000000000000000`. Registration is on <https://www.simnow.com.cn>
(the same portal distributes the official CTPAPI SDK).

| Environment | Trading front | Market-data front |
|-------------|---------------|-------------------|
| 7x24 (always on) | `tcp://182.254.243.31:40001` | `tcp://182.254.243.31:40011` |
| 仿真 (follows real sessions) | `tcp://182.254.243.31:30001` | `tcp://182.254.243.31:30011` |
| 仿真 (alternate) | `tcp://182.254.243.31:30002` | `tcp://182.254.243.31:30012` |
| 仿真 (alternate) | `tcp://182.254.243.31:30003` | `tcp://182.254.243.31:30013` |

Note: the 7x24 environment replays the most recent trading day's tick data
around the clock; the 仿真 environments follow real trading sessions and real
market data. SimNow matching is market-maker style against the live order
book, so partial fills occur only in specific instruments.

## Files in this directory

| Path | What it is |
|------|------------|
| `pdf/CTP客户端开发指南.pdf` | Official CTP client development guide (authentication flow, API usage) |
| `pdf/综合交易平台API技术开发指南.pdf` | Official API technical development guide |
| `pdf/综合交易平台API开发常见问题列表.pdf` | Official FAQ (common pitfalls, flow control, reconnects) |
| `pdf/综合交易平台交易API特别说明.pdf` | Official trading-API special notes (order/quote semantics) |
| `pdf/CTP期权保证金手续费算法说明.pdf` | Official option margin and fee algorithm notes |
| `pdf/期货交易数据交换协议.pdf` | FTD futures trading data exchange protocol (wire-level background) |
| `sdk/headers/ThostFtdcTraderApi.h` | Trading API interface (requests and callbacks), CTP 6.7.11 |
| `sdk/headers/ThostFtdcMdApi.h` | Market-data API interface, CTP 6.7.11 |
| `sdk/headers/ThostFtdcUserApiStruct.h` | All request/response struct definitions |
| `sdk/headers/ThostFtdcUserApiDataType.h` | All enum and field type definitions |
| `sdk/error.dtd`, `sdk/error.xml` | Venue error code table (lookup `ErrorID` -> message) |
| `sdk/xml/ctp-6.7.11.xml` | Machine-readable data definition for the full API surface |
| `sdk/lib/linux64/*.so` | Linux x64 runtime libraries (`thosttraderapi_se`, `thostmduserapi_se`) |
| `sdk/v6.7.11_traderapi升级说明.txt` | Official 6.7.11 upgrade notes |
| `ctpapi-downloads-and-docs.md` | Markdown capture of the openctp CTPAPI page (SDK index, doc links, SimNow fronts) |
| `openctp-sim-environment.md` | Markdown capture of the openctp simulation environment page (7x24 TTS alternative to SimNow) |

Each markdown file carries a `<!-- Source: ... Fetched: ... -->` provenance
header, matching the convention in `docs/um-futures-testnet/`.

## What was deliberately not vendored

- Windows `thosttraderapi_se.dll/.lib` and `thostmduserapi_se.dll/.lib`
  (32-bit and 64-bit) and the two 看穿式 (client information collection)
  compliance PDFs: download the full official zip
  <http://www.openctp.cn/download/CTPAPI/CTP/ctp_6.7.11.zip> when a Windows
  build is needed. The `se` (看穿式, see-through) variant is mandatory for
  production; SimNow accepts the same SDK.
- Older SDK versions (6.3.15 through 6.7.10) are listed on
  <http://www.openctp.cn/CTPAPI.html> if a broker's production counter runs
  an older CTP version.

## Reference implementations

- <https://github.com/vnpy/vnpy_ctp> — C++ CTP 6.7.11 wrapper used by
  VeighNa; a good map of which callbacks matter in practice.
- <https://github.com/openctp/openctp> — CTPAPI ecosystem; `demo/` has CTP
  demo source code, and its TTS system offers a 7x24 CTPAPI-compatible
  simulation environment (see `openctp-sim-environment.md`). Note that
  connecting to TTS requires replacing the CTP dll/so with the TTS
  compatible ones; the official CTP libraries fail with error 4097 there.

## Known gaps

- The official 上期技术 site (www.sfit.com.cn) requires login for downloads;
  the openctp mirror of SDK versions and official PDFs was used instead.
  Verify against SimNow when in doubt.
- CTPAPI behavior details (flow control limits, front failover, session
  heartbeat parameters) live in the PDFs, not in the headers.
