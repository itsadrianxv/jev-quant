# Binance UM Futures (USDS-M) API documentation

Official Binance USDⓈ-M Futures API documentation fetched as markdown on
**2026-09-25**, for integrating the trading system with the UM Futures
testnet. This complements `docs/binance-testnet/` (Spot testnet docs).

## Testnet endpoints (from `general-info.md`)

| Transport | Testnet base URL |
|-----------|------------------|
| REST      | `https://demo-fapi.binance.com` |
| WebSocket | `wss://demo-fstream.binance.com` |

Production bases are `https://fapi.binance.com` and `wss://fstream.binance.com`;
the API surface is otherwise the same.

## Files

| File | Source page |
|------|-------------|
| `introduction.md` | [Introduction](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/Introduction) |
| `quick-start.md` | [Quick Start](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/quick-start) |
| `general-info.md` | [General Info](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/general-info) — base endpoints, signing, limits, **Testnet API Information** |
| `websocket-api-general-info.md` | [WebSocket API General Info](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/websocket-api-general-info) |
| `websocket-market-streams/` | [WebSocket Market Streams](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/websocket-market-streams) — connect, live subscribing, local order book management, change notice |
| `user-data-streams.md` | [User Data Streams](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/user-data-streams) — listen key and all account/order events with payloads |
| `common-definition.md` | [Public Endpoints Info](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/common-definition) |
| `error-code.md` | [Error Codes](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/error-code) |
| `change-log.md` | [Change Log](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/change-log) |
| `faq/stp-faq.md` | [STP FAQ](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/faq/stp-faq) |
| `api-endpoint-index.md` | Extracted from [llms.txt](https://developers.binance.com/en/docs/llms.txt) — full endpoint list for the UM Futures REST API, WebSocket API, and WebSocket Market Streams |
| `api-endpoint-reference-skill.md` | Binance Skills Hub skill `derivatives-trading-usds-futures` — concise endpoint reference with parameters and enums (see note below) |
| `refresh-docs.ps1` | Script to re-fetch the pages above (needs a reachable proxy) |

Each file carries a `<!-- Source: ... Fetched: ... -->` provenance header.

## Known gaps

- The per-endpoint REST/WS API reference (full parameter documentation, e.g.
  `POST /fapi/v1/order`) is rendered client-side on the site from OpenAPI
  specs and has **no markdown endpoint**. Browse it at
  <https://developers.binance.com/en/docs/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/rest-api>
  in a normal browser. As a substitute, `api-endpoint-index.md` lists every
  endpoint and `api-endpoint-reference-skill.md` documents each endpoint's
  parameters and enums.
- `api-endpoint-reference-skill.md` is the official Skills Hub content
  (authored by Binance). The canonical repository
  `github.com/binance/binance-skills-hub` does not list this skill at the time
  of fetching, so it was mirrored from a feed mirror of the hub; verify
  against the hub when it is published there.
- The `websocket-market-streams` index page and its `Connect` page have no
  markdown endpoint; connection details are covered inside
  `websocket-api-general-info.md` and `user-data-streams.md`.

## How these files were fetched

`developers.binance.com` is DNS-polluted on the development network, so pages
were fetched through the site's official Agent Native markdown endpoints
(`llms.txt`) via a local proxy. `refresh-docs.ps1` repeats the process.
