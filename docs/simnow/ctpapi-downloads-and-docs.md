<!-- Source: http://www.openctp.cn/CTPAPI.html
     Fetched: 2026-09-26 (content condensed to integration-relevant sections) -->

# CTPAPI downloads and official documentation (openctp capture)

CTP is the futures/options trading counter built by 上期技术, a technology
subsidiary of 上期所 (SHFE). CTPAPI is its open client API. The official
download portal is <https://www.simnow.com.cn> (SimNow registration uses the
same portal). Because that portal requires login, the openctp community site
mirrors every SDK version and the official PDFs, and renders the four key
headers as browsable HTML.

## SDK versions (CTP futures API)

Available from <http://www.openctp.cn/download/CTPAPI/CTP/>:

6.3.15, 6.3.19_P1, 6.5.1, 6.6.1_P1, 6.6.7, 6.6.9, 6.7.0, 6.7.1, 6.7.2,
6.7.7, 6.7.8, 6.7.9, 6.7.9_P1, 6.7.10, **6.7.11** (vendored in this repo,
see `sdk/`).

Each version ships as a zip containing per-platform subdirectories
(Windows 32/64 dll+lib, Linux x64 `.so`) with the four headers shared across
platforms:

- `ThostFtdcTraderApi.h` — trading API: `ReqUserLogin`, `ReqOrderInsert`,
  `ReqOrderAction` (cancel), and the `OnRsp*`/`OnRtn*` callbacks
- `ThostFtdcMdApi.h` — market-data API: subscribe, `OnRspDepthMarketData`
- `ThostFtdcUserApiStruct.h` — all structs (e.g. `CThostFtdcInputOrderField`,
  `CThostFtdcTradeField`, `CThostFtdcOrderField`,
  `CThostFtdcDepthMarketDataField`)
- `ThostFtdcUserApiDataType.h` — all enums and fixed-width field typedefs

A machine-readable definition of the whole surface is published per version
as `ctp-<version>.xml` (vendored as `sdk/xml/ctp-6.7.11.xml`), useful for
generating bindings or lookup tables.

## Official PDF documentation

All under <http://www.openctp.cn/download/docs/>:

- `CTP客户端开发指南.pdf` — client development guide: API lifecycle,
  authentication (穿透式 `ReqAuthenticate`), front address failover, and
  callback threading model
- `综合交易平台API技术开发指南.pdf` — technical development guide
- `综合交易平台API开发常见问题列表.pdf` — FAQ: flow control (1 tick / sec,
  30 reqs / sec etc.), error handling, reconnection semantics
- `综合交易平台交易API特别说明.pdf` — trading API special notes: order
  status transitions, `OrderSysID`/`OrderLocalID`, 今昨仓 rules
- `CTP期权保证金手续费算法说明.pdf` — option margin and fee algorithms
- `期货交易数据交换协议.pdf` — FTD wire protocol background

## SimNow front addresses (official simulation environment)

BrokerID `9999`, AppID `simnow_client_test`, AuthCode `0000000000000000`
(same credentials for all SimNow environments).

| Environment | Trading fronts | Market-data fronts |
|-------------|----------------|--------------------|
| 7x24 | `tcp://182.254.243.31:40001` | `tcp://182.254.243.31:40011` |
| 仿真 1–3 | `tcp://182.254.243.31:30001`–`30003` | `tcp://182.254.243.31:30011`–`30013` |

## CTPAPI-compatible alternatives

- openctp TTS: a 7x24 CTPAPI-compatible simulation system (swap the CTP
  dll/so for TTS ones; official CTP libraries fail with error 4097 there)
- LocalCTP: embedded CTPAPI-compatible simulation with no server
- openctp also provides CTPAPI-compatible interfaces for stock counters
  (XTP, TORA, EMT, ...) and other-language bindings (Python, Rust, Go, Java,
  C, C#)
