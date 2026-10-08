# C++20 固定 worker 数量线程池

一个使用无界 FIFO 队列的 C++20 线程池，支持泛型任务提交、`std::future` 结果反馈、优雅关闭和立即关闭。核心功能、37 项行为测试以及本机 Release 性能验证已完成。

## 使用

包含 `thread_pool.hpp`，并链接静态库目标 `thread_pool::thread_pool`：

```cmake
add_subdirectory(path/to/thread_pool_project)
target_link_libraries(your_app PRIVATE thread_pool::thread_pool)
```

```cpp
#include "thread_pool.hpp"

int main() {
    thread_pool pool(2);
    auto result = pool.submit([](int a, int b) { return a + b; }, 3, 4);
    return result.get() == 7 ? 0 : 1;
} // 析构时优雅关闭，排空队列并等待所有 worker 退出。
```

省略 worker 数量时使用硬件并发数，无法获取时采用 1；显式传入 0 抛 `std::invalid_argument`。callable 和参数按值保存，借用对象使用 `std::ref` / `std::cref`，并由调用方保证其生命周期。

任务异常由对应的 `future.get()` 按原类型反馈。`shutdown()` 排空队列；`shutdown_now()` 丢弃尚未领取的任务，其 future 反馈 `broken_promise`。两种关闭都会等待已领取任务和所有 worker 结束，支持池外线程并发或重复调用，首次生效的关闭方式保持到底。

FIFO 保证领取顺序，不保证多个 worker 的调用开始或完成顺序。本池 worker 调用本池关闭接口抛 `std::logic_error`。析构前须结束其他线程对池的访问。完整规则见 [API 契约](docs/API.md)。

本版本不包含动态 worker、有界队列、任务窃取、优先级、暂停恢复、单任务取消或全池等待接口。

## 构建与验证

本机环境为 Windows / MSYS2 UCRT64、GNU GCC 16.2.0、CMake 3.25 及以上、Ninja。项目使用 C++20，禁用 GNU 扩展；默认构建为 Debug。在项目根目录运行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
# 独立运行 CTest 时需要 UCRT64 运行库目录。
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
ctest --test-dir build/ucrt64-debug --output-on-failure
```

预设指定 `C:/msys64/ucrt64/bin/g++.exe` 和 `C:/msys64/ucrt64/bin/ninja.exe`，检查 GCC 版本；产物位于 `build/ucrt64-debug/`。预设和上述 PATH 设置均不修改系统环境。编辑器与工具链说明见 [本机开发约定](CLAUDE.md)。

构建包含独立/重复包含头文件检查；CTest 包含基础执行 5 项、优雅关闭 6 项、立即关闭 6 项、关闭竞争与自等待 6 项、生命周期与异常安全 12 项，以及 submit 和真实 worker 两项回归，共 37 项。覆盖范围和未触发路径见 [实现与验证记录](docs/IMPLEMENTATION.md)。

## 性能复现

```powershell
cmake --preset ucrt64-release
cmake --build --preset ucrt64-release
& ./benchmarks/run.ps1 -Rounds 10
& ./benchmarks/run.ps1 -Suite advanced -Rounds 10
```

Release 预设启用独立基准，基准不加入 CTest。基础组 42 个配置、扩展组 84 个配置，各有预热及至少 10 次正式测量；扩展覆盖任务粒度、入队与消费分离、积压立即关闭。原始 CSV、环境、源码哈希和波动统计保存在 `benchmarks/results/`。

性能数字只适用于记录中的机器和负载；平均提交、完整批次吞吐、反馈观察及关闭等待使用不同计时边界。方法与本机实测结果见 [性能记录](docs/PERFORMANCE.md)。

## 项目结构与文档

| 路径 | 内容 |
| --- | --- |
| [include/thread_pool.hpp](include/thread_pool.hpp) | 公共接口、Task 类型擦除、submit 模板与 worker 生命周期 |
| [src/thread_pool.cpp](src/thread_pool.cpp) | 池构造/析构、关闭流程、enqueue 与 worker 循环 |
| [tests/](tests/) | 行为测试、受控同步与异常安全验证 |
| [benchmarks/](benchmarks/) | Release 基准、运行/汇总脚本与历史实测记录 |
| [API.md](docs/API.md) | 调用语义、关闭边界与异常反馈 |
| [DESIGN.md](docs/DESIGN.md) | 状态、锁职责、任务所有权与生命周期 |
| [IMPLEMENTATION.md](docs/IMPLEMENTATION.md) | 实现状态、测试范围与历史实施记录 |
| [PERFORMANCE.md](docs/PERFORMANCE.md) | 性能方法、计时边界与实测数据 |
| [REVIEW.md](docs/REVIEW.md) | 前三轮精简的审核历史 |
| [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md) | 本机开发环境与代理约定 |
