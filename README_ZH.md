# jev-quant

使用 TypeSafe Jev 模型的 C++ 交易系统。本项目是我学习 C++ 量化开发技术栈过程中搭来玩的项目，包括以下内容：
1. 一个限价订单簿：根据增量行情逐步填充，并在必要时通过快照同步
2. 内存池：避免在热路径上进行动态内存分配
3. SPSC 无锁队列：用于线程间通信
4. 异步日志模块：将耗时的 I/O 操作移出热路径
5. 性能测试与分析：使用时间戳测量热路径的耗时，并通过 Callgrind 进行性能剖析

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
