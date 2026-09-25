<!-- Source: https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/websocket-market-streams
     Fetched: 2026-09-25 via r.jina.ai (developers.binance.com unreachable directly) -->
# Build with Binance APIs

Access market data, trading, account, and real-time interfaces to build bots, backends, dashboards, and institutional integrations on Binance.

[Get started](https://developers.binance.com/en/docs/introduction)[Explore APIs](https://developers.binance.com/en/docs/catalog)

Python JavaScript Java Rust Go

```
import requests

# Public endpoint — no API key needed
resp = requests.get(
    "https://api.binance.com/api/v3/ticker/price",
    params={"symbol": "BTCUSDT"},
)
print(resp.json())
# → {"symbol": "BTCUSDT", "price": "84523.00"}
```

[Browse official connectors for all languages](https://developers.binance.com/en/docs/sdks-tools/overview#official-api-connectors)

Choose your path

## Start from the right place

Whether you are new to Binance APIs, exploring product families, or looking for tooling, start with the path that matches your workflow.

### Get started

Best for first-time integrations and understanding the platform.

[Introduction](https://developers.binance.com/en/docs/introduction)[Authentication & security](https://developers.binance.com/en/docs/introduction#authentication-and-security)[Environments](https://developers.binance.com/en/docs/introduction#environments)

### Explore products

Jump directly into the most commonly used Binance product families.

[Spot Trading](https://developers.binance.com/en/docs/products/spot/rest-api)[USDⓈ-M Futures](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/Introduction)[Wallet](https://developers.binance.com/en/docs/products/wallet/Introduction)

### Build with SDKs & tools

Use supported tooling, connectors, collections, and implementation resources.

[Connectors](https://developers.binance.com/en/docs/sdks-tools/overview#official-api-connectors)[Developer tools](https://developers.binance.com/en/docs/sdks-tools/overview#developer-tools)[Postman collections](https://developers.binance.com/en/docs/sdks-tools/overview#postman-collections)

### Agent Native

Integrate with AI agents using machine-readable docs, agent APIs, and MCP.

[Overview](https://developers.binance.com/en/docs/agent-native/overview)[llms.txt](https://developers.binance.com/en/docs/agent-native/llms-txt)

Key docs

## Production essentials

Cross-product documentation that is worth checking before you ship an integration.

[Authentication API keys, signing, and authenticated requests.](https://developers.binance.com/en/docs/introduction#authentication-and-security)[Rate limits Weights, request limits, and reliability guidance.](https://developers.binance.com/en/docs/introduction#rate-limits-and-reliability)[Environments Production and testing environments across products.](https://developers.binance.com/en/docs/introduction#environments)[Support Where to get help and follow official updates.](https://developers.binance.com/en/docs/introduction#getting-help)

Community

## Stay connected

Follow updates, report issues, and connect with other developers building on Binance.

[GitHub Connectors, sample code, and issue tracking.](https://github.com/binance)[Telegram Join the official Binance developer channel.](https://t.me/binance_api_english)[Developer Community Ask questions and discuss integrations with other builders.](https://dev.binance.vision/)[Announcements API deprecations, upgrades, and notices.](https://www.binance.com/en/support/announcement)
