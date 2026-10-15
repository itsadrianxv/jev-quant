# jev-quant

[中文](./README_ZH.md)

jev-quant is a C++ trading system leveraging Jev, a TypeSafe systems one model. I built this project mainly for tinkering with the C++ tech stack required for quantitative developer roles & having fun with the Jev model. That's why the project features a bunch of HFT-related stuff, including:
1. A limit order book that builds incrementally from market updates & syncs to snapshots if necessary.
2. A memory pool that prevents dynamic memory allocation on the hot path.
3. SPSC lock-free queues that facilitate communication between threads.
4. Asynchronous logging module that separates the slow I/O operations from the hot path.
5. Performance benchmarking on the hot path using timestamps and profiling using Callgrind. 

Now supporting simulated SHFE futures trading through SimNow and the official CTP SDK. See [the SimNow operations guide](docs/simnow/adapter-operations.md).

Overall this is a pet project that I build to learn all these tech, and I am very very early in this project, so expect bugs :)

## Quick Start

### Prerequisites

- C++20 compatible compiler (GCC 11+, Clang 13+, or MSVC 19.29+; CI uses GCC 13 on Ubuntu 24.04)
- CMake 3.24 or newer
- Ninja 1.11 or newer

### Building with CMake

#### Linux

Install the build dependencies (Debian/Ubuntu):

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ ninja-build libcurl4-openssl-dev nlohmann-json3-dev
```

Configure and build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJEV_ENABLE_SIMEX=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Configuration

Copy `.env.example` to `.env` and set your OpenRouter API key:

```bash
cp .env.example .env
```

### Run

From the repository root, run:

```bash
./build/jev_trading
```

The process runs in the foreground and shuts down on `Ctrl+C`.

### Simex venue

The optional simex adapter connects to a separate localhost venue process and uses the real OpenRouter Jev provider. It is enabled by default and expects the compatible simex checkout at `../simex`; use `-DJEV_ENABLE_SIMEX=OFF` for a Binance-only build. See [the simex integration guide](docs/simex-venue.md) for building both processes, configuration, live validation, and current limits.

## Layout

```text
src/
tests/
```

## License

MIT
