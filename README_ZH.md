# jev-quant

jev-quant 是一个 C++ 交易系统，利用 Jev（一个擅长即时决策的 TypeSafe 模型）进行交易；即时决策这一特性使它尤其适合实盘交易。

现在支持通过 SimNow 和官方 CTP SDK 进行 SHFE 期货模拟交易。请参阅 [SimNow 操作指南](docs/simnow/adapter-operations.md)。

## 快速开始

### 先决条件

- 兼容 C++20 的编译器（GCC 11+、Clang 13+ 或 MSVC 19.29+；CI 使用 Ubuntu 24.04 上的 GCC 13）
- CMake 3.24 或更新版本
- Ninja 1.11 或更新版本

### 使用 CMake 构建

#### Linux

安装构建依赖（Debian/Ubuntu）：

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ ninja-build libcurl4-openssl-dev nlohmann-json3-dev
```

配置并构建：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJEV_ENABLE_SIMEX=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### 配置

将 `.env.example` 复制为 `.env`，并设置 OpenRouter API 密钥：

```bash
cp .env.example .env
```

### 运行

在仓库根目录运行：

```bash
./build/jev_trading
```

程序会在前台运行，按 `Ctrl+C` 关闭。

### Simex 交易场所

可选的 simex 适配器连接到单独运行在本机的交易场所进程，并使用真实的 OpenRouter Jev 提供方。该适配器默认启用，并要求兼容的 simex 检出目录位于 `../simex`；使用 `-DJEV_ENABLE_SIMEX=OFF` 可构建仅支持 Binance 的版本。关于两个进程的构建、配置、实盘验证和当前限制，请参阅 [simex 集成指南](docs/simex-venue.md)。

## 目录结构

```text
src/
tests/
```

## 说明

本项目还处于非常早期的阶段，预计会存在缺陷。

## 许可证

MIT
