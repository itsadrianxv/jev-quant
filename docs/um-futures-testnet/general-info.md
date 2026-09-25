<!-- Source: https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/general-info
     Fetched: 2026-09-25 via r.jina.ai (developers.binance.com unreachable directly) -->
## General API Information

*   Some endpoints will require an API Key. Please refer to [this page](https://www.binance.com/en/support/articles/360002502072)
*   The base endpoint is: **[https://fapi.binance.com](https://fapi.binance.com/)**
*   All endpoints return either a JSON object or array.
*   Data is returned in **ascending** order. Oldest first, newest last.
*   All time and timestamp related fields are in milliseconds.
*   All data types adopt definition in JAVA.

### Testnet API Information

*   Most of the endpoints can be used in the testnet platform.
*   The REST base url for **testnet** is "[https://demo-fapi.binance.com](https://demo-fapi.binance.com/)"
*   The Websocket base url for **testnet** is "wss://demo-fstream.binance.com"

* * *

## General Information on Endpoints

*   For `GET` endpoints, parameters must be sent as a `query string`.
*   For `POST`, `PUT`, and `DELETE` endpoints, the parameters may be sent as a `query string` or in the `request body` with content type `application/x-www-form-urlencoded`. You may mix parameters between both the `query string` and `request body` if you wish to do so.
*   Parameters may be sent in any order.
*   If a parameter sent in both the `query string` and `request body`, the `query string` parameter will be used.

### HTTP Return Codes

*   HTTP `4XX` return codes are used for for malformed requests; the issue is on the sender's side.
*   HTTP `403` return code is used when the WAF Limit (Web Application Firewall) has been violated.
*   HTTP `408` return code is used when a timeout has occurred while waiting for a response from the backend server.
*   HTTP `429` return code is used when breaking a request rate limit.
*   HTTP `418` return code is used when an IP has been auto-banned for continuing to send requests after receiving `429` codes.
*   HTTP `5XX` return codes are used for internal errors; the issue is on Binance's side.
    1.   If there is an error message **"Request occur unknown error."**, please retry later.

*   HTTP `503` return code is used when:
    1.   If there is an error message **"Unknown error, please check your request or try again later."** returned in the response, the API successfully sent the request but not get a response within the timeout period.  
It is important to **NOT** treat this as a failure operation; the execution status is **UNKNOWN** and could have been a success;
    2.   If there is an error message **"Service Unavailable."** returned in the response, it means this is a failure API operation and the service might be unavailable at the moment, you need to retry later.
    3.   If there is an error message **"Internal error; unable to process your request. Please try again."** returned in the response, it means this is a failure API operation and you can resend your request if you need.
    4.   If the response contains the error message **"Request throttled by system-level protection. Reduce-only/close-position orders are exempt. Please try again." (-1008)**, This indicates the node has exceeded its maximum concurrency and is temporarily throttled. Close-position, reduce-only, and cancel orders are exempt and will not receive this error.

### HTTP 503 Status: Message Variants & Handling

#### A. “Unknown error, please check your request or try again later.” (Execution status **unknown**)

*   **Meaning**: Request accepted but no response before timeout; **execution may have succeeded**.
*   **Handling**:
    *   **Do not treat as immediate failure**; first verify via **WebSocket updates** or **orderId queries** to avoid duplicates.
    *   During peaks, prefer **single orders** over batch to reduce uncertainty.

*   **Rate-limit counting**: **May or may not** count, check header to verify rate limit info

#### B. “Service Unavailable.” (Failure)

*   **Meaning**: Service temporarily unavailable; **100% failure**.
*   **Handling**: **Retry with exponential backoff** (e.g., 200ms → 400ms → 800ms, max 3–5 attempts).
*   **Rate-limit counting**: **not counted**

#### C. “Request throttled by system-level protection. Reduce-only/close-position orders are exempt. Please try again.” (**-1008**, Failure)

*   **Meaning**: System overload; **100% failure**.
*   **Handling**: **Retry with backoff** and **reduce concurrency**;
*   **Applicable endpoints**:
    *   `POST /fapi/v1/order`
    *   `POST /fapi/v1/batchOrders`
    *   `POST /fapi/v1/order/test`

*   **Rate-limit counting**: **Not counted** (overload protection).
*   **Exception integrated here**: When a request **reduces exposure** (Reduce-only / Close-position: `closePosition = true`, or `positionSide = BOTH` with `reduceOnly = true`, or `LONG+SELL`, or `SHORT+BUY`), it is **not affected or prioritized under -1008** to ensure risk reduction.
    *   Covered endpoints: `POST /fapi/v1/order`、`POST /fapi/v1/batchOrders` (when parameters satisfy the condition)

### Error Codes and Messages

*   Any endpoint can return an ERROR

> **_The error payload is as follows:_**

*   Specific error codes and messages defined in [Error Codes](https://developers.binance.com/en/docs/products/derivatives-trading-usds-futures/general-info#error-codes).

* * *

## SDK and Code Demonstration

**Disclaimer:**

*   The following SDKs are provided by partners and users, and are **not officially** produced. They are only used to help users become familiar with the API endpoint. Please use it with caution and expand R&D according to your own situation.
*   Binance does not make any commitment to the safety and performance of the SDKs, nor will be liable for the risks or even losses caused by using the SDKs.

### Python3

**SDK:** To get the provided SDK for Binance Futures Connector, please visit [https://github.com/binance/binance-connector-python](https://github.com/binance/binance-connector-python), or use the command below: `pip install binance-sdk-derivatives-trading-usds-futures`

### Java

To get the provided SDK for Binance Futures, please visit [https://github.com/binance/binance-connector-java](https://github.com/binance/binance-connector-java), or use the command below: `git clone https://github.com/binance/binance-connector-java.git`

* * *

## LIMITS

*   The `/fapi/v1/exchangeInfo``rateLimits` array contains objects related to the exchange's `RAW_REQUEST`, `REQUEST_WEIGHT`, and `ORDER` rate limits. These are further defined in the `ENUM definitions` section under `Rate limiters (rateLimitType)`.
*   A `429` will be returned when either rate limit is violated.

### IP Limits

*   Every request will contain `X-MBX-USED-WEIGHT-(intervalNum)(intervalLetter)` in the response headers which has the current used weight for the IP for all request rate limiters defined.
*   Each route has a `weight` which determines for the number of requests each endpoint counts for. Heavier endpoints and endpoints that do operations on multiple symbols will have a heavier `weight`.
*   When a 429 is received, it's your obligation as an API to back off and not spam the API.
*   **Repeatedly violating rate limits and/or failing to back off after receiving 429s will result in an automated IP ban (HTTP status 418).**
*   IP bans are tracked and **scale in duration** for repeat offenders, **from 2 minutes to 3 days**.
*   **The limits on the API are based on the IPs, not the API keys.**

### Order Rate Limits

*   Every order response will contain a `X-MBX-ORDER-COUNT-(intervalNum)(intervalLetter)` header which has the current order count for the account for all order rate limiters defined.
*   Rejected/unsuccessful orders are not guaranteed to have `X-MBX-ORDER-COUNT-**` headers in the response.
*   **The order rate limit is counted against each account**.

* * *

## Endpoint Security Type

*   Each endpoint has a security type that determines the how you will interact with it.
*   API-keys are passed into the Rest API via the `X-MBX-APIKEY` header.
*   API-keys and secret-keys **are case sensitive**.
*   API-keys can be configured to only access certain types of secure endpoints. For example, one API-key could be used for TRADE only, while another API-key can access everything except for TRADE routes.
*   By default, API-keys can access all secure routes.

| Security Type | Description |
| --- | --- |
| NONE | Endpoint can be accessed freely. |
| TRADE | Endpoint requires sending a valid API-Key and signature. |
| USER_DATA | Endpoint requires sending a valid API-Key and signature. |
| USER_STREAM | Endpoint requires sending a valid API-Key. |
| MARKET_DATA | Endpoint requires sending a valid API-Key. |

*   `TRADE` and `USER_DATA` endpoints are `SIGNED` endpoints.

### SIGNED (TRADE and USER_DATA) Endpoint Security

*   `SIGNED` endpoints require an additional parameter, `signature`, to be sent in the `query string` or `request body`.
*   Endpoints use `HMAC SHA256` signatures. The `HMAC SHA256 signature` is a keyed `HMAC SHA256` operation. Use your `secretKey` as the key and `totalParams` as the value for the HMAC operation.
*   The `signature` is **not case sensitive**.
*   Please make sure the `signature` is the end part of your `query string` or `request body`.
*   `totalParams` is defined as the `query string` concatenated with the `request body`.

### Timing Security

*   A `SIGNED` endpoint also requires a parameter, `timestamp`, to be sent which should be the millisecond timestamp of when the request was created and sent.
*   An additional parameter, `recvWindow`, may be sent to specify the number of milliseconds after `timestamp` the request is valid for. If `recvWindow` is not sent, **it defaults to 5000**.

> The logic is as follows:

**Serious trading is about timing.** Networks can be unstable and unreliable, which can lead to requests taking varying amounts of time to reach the servers. With `recvWindow`, you can specify that the request must be processed within a certain number of milliseconds or be rejected by the server.

### SIGNED Endpoint Examples for POST /fapi/v1/order - HMAC Keys

Here is a step-by-step example of how to send a vaild signed payload from the Linux command line using `echo`, `openssl`, and `curl`.

| Key | Value |
| --- | --- |
| apiKey | dbefbc809e3e83c283a984c3a1459732ea7db1360ca80c5c2c8867408d28cc83 |
| secretKey | 2b5eb11e18796d12d88f13dc27dbbd02c2cc51ff7059765ed9821957d82bb4d9 |

| Parameter | Value |
| --- | --- |
| symbol | BTCUSDT |
| side | BUY |
| type | LIMIT |
| timeInForce | GTC |
| quantity | 1 |
| price | 9000 |
| recvWindow | 5000 |
| timestamp | 1591702613943 |

#### Example 1: As a query string

> **Example 1**

> **HMAC SHA256 signature:**

> **curl command:**

*   **queryString:**

symbol=BTCUSDT  
&side=BUY  
&type=LIMIT  
&timeInForce=GTC  
&quantity=1  
&price=9000  
&recvWindow=5000  
&timestamp=1591702613943

#### Example 2: As a request body

> **Example 2**

> **HMAC SHA256 signature:**

> **curl command:**

*   **requestBody:**

symbol=BTCUSDT  
&side=BUY  
&type=LIMIT  
&timeInForce=GTC  
&quantity=1  
&price=9000  
&recvWindow=5000  
&timestamp=1591702613943

#### Example 3: Mixed query string and request body

> **Example 3**

> **HMAC SHA256 signature:**

> **curl command:**

*   **queryString:** symbol=BTCUSDT&side=BUY&type=LIMIT&timeInForce=GTC
*   **requestBody:** quantity=1&price=9000&recvWindow=5000&timestamp= 1591702613943

Note that the signature is different in example 3.  
There is no & between "GTC" and "quantity=1".

### SIGNED Endpoint Examples for POST /fapi/v1/order - RSA Keys

*   This will be a step by step process how to create the signature payload to send a valid signed payload.
*   We support `PKCS#8` currently.
*   To get your API key, you need to upload your RSA Public Key to your account and a corresponding API key will be provided for you.

For this example, the private key will be referenced as `test-prv-key.pem`

| Key | Value |
| --- | --- |
| apiKey | vE3BDAL1gP1UaexugRLtteaAHg3UO8Nza20uexEuW1Kh3tVwQfFHdAiyjjY428o2 |

| Parameter | Value |
| --- | --- |
| symbol | BTCUSDT |
| side | SELL |
| type | MARKET |
| quantity | 1.23 |
| recvWindow | 9999999 |
| timestamp | 1671090801999 |

> **Signature payload (with the listed parameters):**

**Step 1: Construct the payload**

Arrange the list of parameters into a string. Separate each parameter with a `&`.

**Step 2: Compute the signature:**

2.1 - Encode signature payload as ASCII data.

> **Step 2.2**

2.2 - Sign payload using RSASSA-PKCS1-v1_5 algorithm with SHA-256 hash function.

> **Step 2.3**

2.3 - Encode output as base64 string.

> **Step 2.4**

2.4 - Delete any newlines in the signature.

> **Step 2.5**

2.5 - Since the signature may contain `/` and `=`, this could cause issues with sending the request. So the signature has to be URL encoded.

> **Step 2.6**

2.6 - curl command

> **Bash script**

A sample Bash script containing similar steps is available in the right side.

* * *

## Postman Collections

There is now a Postman collection containing the API endpoints for quick and easy use.

For more information please refer to this page: [Binance API Postman](https://github.com/binance-exchange/binance-api-postman)
