# User Data Streams

## Connect

- The base API endpoint is: **https://fapi.binance.com**
- A User Data Stream `listenKey` is valid for 60 minutes after creation.
- Doing a `PUT` on a `listenKey` will extend its validity for 60 minutes, if response `-1125` error
  "This listenKey does not exist." Please use `POST /fapi/v1/listenKey` to recreate `listenKey`.
- Doing a `DELETE` on a `listenKey` will close the stream and invalidate the `listenKey`.
- Doing a `POST` on an account with an active `listenKey` will return the currently active
  `listenKey` and extend its validity for 60 minutes.
- The connection method for Websocket:
  - Base Url: **wss://fstream.binance.com/private**
  - User Data Streams are accessed at **/ws/\<listenKey\>**
  - Example:
    `wss://fstream.binance.com/private/ws/XaEAKTsQSRLZAGH9tuIu37plSRsdjmlAVBoNYPUITlTAko1WI22PgmBMpI1rS8Yh`
- **Message Ordering Guarantee**:
  - For the same user on a single Websocket connection, messages of the **same event type** (e.g.,
    `ACCOUNT_UPDATE`, `ORDER_TRADE_UPDATE`) are **strictly ordered** by both `T` (transaction time
    from Matching Engine) and `E` (event time when the message is generated).
  - **Recommended**: Use the `E` field for ordering updates, especially when:
    - Comparing events across different event types (e.g., `ORDER_TRADE_UPDATE` vs market data
      streams like `aggTrade`).
    - Events from different services may share the same `T` but have different `E` values due to
      processing timing.
    - For the same event type on the same connection, both `T` and `E` remain strictly ordered, so
      either field can be used reliably.
- A single connection is only valid for 24 hours; expect to be disconnected at the 24 hour mark
- Considering that RESTful endpoints may experience query delays under volatile market conditions,
  we strongly recommend prioritizing Websocket user data stream messages for retrieving orders,
  positions, and other information.

---

## Event: Margin Call

### Description

- When the user's position risk ratio is too high, this stream will be pushed.
- This message is only used as risk guidance information and is not recommended for investment
  strategies.
- In the case of a highly volatile market, there may be the possibility that the user's position has
  been liquidated at the same time when this stream is pushed out.

### Event Name

`MARGIN_CALL`

### Payload

> **Schema:**
> [`marginCall`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#margincall)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="marginCall"
/>

---

## Event: Balance and Position Update

### Description

Event type is `ACCOUNT_UPDATE`.

- When balance or position get updated, this event will be pushed.
  - `ACCOUNT_UPDATE` will be pushed only when update happens on user's account, including changes on
    balances, positions, or margin type.
  - Unfilled orders or cancelled orders will not make the event `ACCOUNT_UPDATE` pushed, since
    there's no change on positions.
  - "position" in `ACCOUNT_UPDATE`: Only symbols of changed positions will be pushed.

- When "FUNDING FEE" changes to the user's balance, the event will be pushed with the brief message:
  - When "FUNDING FEE" occurs in a **crossed position**, `ACCOUNT_UPDATE` will be pushed with the
    balance `B` (including the "FUNDING FEE" asset only) and symbol, without any position `P`
    message.
  - When "FUNDING FEE" occurs in an **isolated position**, `ACCOUNT_UPDATE` will be pushed with the
    balance `B` (including the "FUNDING FEE" asset only), symbol and the relative position message
    `P` (including the isolated position on which the "FUNDING FEE" occurs only, without any other
    position message).

- The field "m" represents the reason type for the event and may shows the following possible types:
  - DEPOSIT
  - WITHDRAW
  - ORDER
  - FUNDING_FEE
  - WITHDRAW_REJECT
  - ADJUSTMENT
  - INSURANCE_CLEAR
  - ADMIN_DEPOSIT
  - ADMIN_WITHDRAW
  - MARGIN_TRANSFER
  - MARGIN_TYPE_CHANGE
  - ASSET_TRANSFER
  - OPTIONS_PREMIUM_FEE
  - OPTIONS_SETTLE_PROFIT
  - AUTO_EXCHANGE
  - COIN_SWAP_DEPOSIT
  - COIN_SWAP_WITHDRAW

- The field "bc" represents the balance change except for PnL and commission.

### Event Name

`ACCOUNT_UPDATE`

### Payload

> **Schema:**
> [`accountUpdate`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#accountupdate)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="accountUpdate"
/>

---

## Event: Order Update

### Description

When new order created, order status changed will push such event. Event type is
`ORDER_TRADE_UPDATE`.

**Side**

- BUY
- SELL

**Order Type**

- LIMIT
- MARKET
- STOP
- STOP_MARKET
- TAKE_PROFIT
- TAKE_PROFIT_MARKET
- TRAILING_STOP_MARKET
- LIQUIDATION

**Execution Type**

- NEW
- CANCELED
- CALCULATED - Liquidation Execution
- EXPIRED
- TRADE
- AMENDMENT - Order Modified

**Order Status**

- NEW
- PARTIALLY_FILLED
- FILLED
- CANCELED
- EXPIRED
- EXPIRED_IN_MATCH

**Time in force**

- GTC
- IOC
- FOK
- GTX

**Working Type**

- MARK_PRICE
- CONTRACT_PRICE

**Liquidation and ADL:**

- If user gets liquidated due to insufficient margin balance:
  - `c` shows as "autoclose-XXX", `X` shows as "NEW"
- If user has enough margin balance but gets ADL:
  - `c` shows as "adl_autoclose", `X` shows as "NEW"

**Expiry Reason**

- `0`: None, the default value
- `1`: Order has expired to prevent users from inadvertently trading against themselves
- `2`: IOC order could not be filled completely, remaining quantity is canceled
- `3`: IOC order could not be filled completely to prevent users from inadvertently trading against
  themselves, remaining quantity is canceled
- `4`: Order has been canceled, as it's knocked out by another higher priority RO (market) order or
  reversed positions would be opened
- `5`: Order has expired when the account was liquidated
- `6`: Order has expired as GTE condition unsatisfied
- `7`: Order has been canceled as the symbol is delisted
- `8`: The initial order has expired after the stop order is triggered
- `9`: Market order could not be filled completely, remaining quantity is canceled

### Event Name

`ORDER_TRADE_UPDATE`

### Payload

> **Schema:**
> [`orderTradeUpdate`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#ordertradeupdate)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="orderTradeUpdate"
/>

---

## Event: Account Configuration Update (previous Leverage Update)

### Description

When the account configuration is changed, the event type will be pushed as `ACCOUNT_CONFIG_UPDATE`.

When the leverage of a trade pair changes, the payload will contain the object `ac` to represent the
account configuration of the trade pair, where `s` represents the specific trade pair and `l`
represents the leverage.

When the user Multi-Assets margin mode changes, the payload will contain the object `ai`
representing the user account configuration, where `j` represents the user Multi-Assets margin mode.

### Event Name

`ACCOUNT_CONFIG_UPDATE`

### Payload

> **Schema:**
> [`accountConfigUpdate`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#accountconfigupdate)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="accountConfigUpdate"
/>

---

## Event: Trade Lite

### Description

Fast trade stream reduces data latency compared to the original `ORDER_TRADE_UPDATE` stream.
However, it only pushes TRADE Execution Type, and fewer data fields.

### Event Name

`TRADE_LITE`

### Payload

> **Schema:**
> [`tradeLite`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#tradelite)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="tradeLite"
/>

---

## Event: Conditional Order Trigger Reject

### Description

`CONDITIONAL_ORDER_TRIGGER_REJECT` update when a triggered TP/SL order got rejected.

### Event Name

`CONDITIONAL_ORDER_TRIGGER_REJECT`

### Payload

> **Schema:**
> [`conditionalOrderTriggerReject`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#conditionalordertriggerreject)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="conditionalOrderTriggerReject"
/>

---

## Event: Strategy Update

### Description

`STRATEGY_UPDATE` update when a strategy is created/cancelled/expired, ...etc.

**Strategy Status**

- NEW
- WORKING
- CANCELLED
- EXPIRED

**opCode**

- 8001: The strategy params have been updated
- 8002: User cancelled the strategy
- 8003: User manually placed or cancelled an order
- 8004: The stop limit of this order reached
- 8005: User position liquidated
- 8006: Max open order limit reached
- 8007: New grid order
- 8008: Margin not enough
- 8009: Price out of bounds
- 8010: Market is closed or paused
- 8011: Close position failed, unable to fill
- 8012: Exceeded the maximum allowable notional value at current leverage
- 8013: Grid expired due to incomplete KYC verification or access from a restricted jurisdiction
- 8014: Violated Futures Trading Quantitative Rules. Strategy stopped
- 8015: User position empty or liquidated

### Event Name

`STRATEGY_UPDATE`

### Payload

> **Schema:**
> [`strategyUpdate`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#strategyupdate)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="strategyUpdate"
/>

---

## Event: Grid Update (Deprecated)

### Description

`GRID_UPDATE` update when a sub order of a grid is filled or partially filled.

**Strategy Status**

- NEW
- WORKING
- CANCELLED
- EXPIRED

### Event Name

`GRID_UPDATE`

### Payload

> **Schema:**
> [`gridUpdate`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#gridupdate)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="gridUpdate"
/>

---

## Event: Algo Order Update

### Description

When new algo order created, order status changed will push such event. Event type is `ALGO_UPDATE`.

**Algo Status**

- `NEW`: The conditional order was successfully placed but has not yet been triggered.
- `CANCELED`: The conditional order has been canceled.
- `TRIGGERING`: The order has met the triggering condition and has been forwarded to the matching
  engine.
- `TRIGGERED`: The order has been successfully placed into the matching engine.
- `FINISHED`: The triggered conditional order has been filled or canceled in the matching engine.
- `REJECTED`: The conditional order has been denied by the matching engine, such as in scenarios of
  margin check failures.
- `EXPIRED`: The conditional order has been canceled by the system. An example would be when a user
  places a GTE_GTC Time-In-Force conditional order but then closes all positions on that symbol,
  resulting in system-led cancellation.

### Event Name

`ALGO_UPDATE`

### Payload

> **Schema:**
> [`algoUpdate`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#algoupdate)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="algoUpdate"
/>

---

## Event: User Data Stream Expired

### Description

When the `listenKey` used for the user data stream turns expired, this event will be pushed.

**Notice:**

- This event is not related to the websocket disconnection.
- This event will be received only when a valid `listenKey` in connection got expired.
- No more user data event will be updated after this event received until a new valid `listenKey`
  used.

### Event Name

`listenKeyExpired`

### Payload

> **Schema:**
> [`listenKeyExpired`](/catalog/core-trading-derivatives-trading-usd-s-m-futures/api/ws-streams/~schemas#listenkeyexpired)

<SchemaEmbed
  spec="derivatives-trading-usds-futures-ws-streams"
  schema="listenKeyExpired"
/>
