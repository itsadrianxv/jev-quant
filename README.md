# jev-quant

jev-quant is a C++ trading system leveraging Jev, a TypeSafe model good at making instant decisions, a trait that makes it especially good at live trading.

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
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
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

## Layout

```text
src/
tests/
```

## License

MIT
