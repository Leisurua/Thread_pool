# C++20 thread_pool

固定 worker、无界 FIFO 队列的 C++20 线程池，支持泛型 submit、future 结果反馈、优雅关闭和立即关闭。项目重点是任务所有权、并发关闭与生命周期安全，当前功能范围固定。

## 主要设计

- **任务封装**：packaged_task / future 与 Task 类型擦除，支持 move-only 对象及 std::ref。
- **双锁关闭**：队列锁保护入队、领取和状态；关闭锁串行化关闭与回收，首次生效的方式保持到底。
- **生命周期安全**：TLS / 私有 RAII 守卫识别 worker，负责人重入检测避免同线程自等待；任务销毁和 join 在队列锁外。
- **线程管理**：jthread 管理线程生命周期，退出由状态与条件变量驱动。

## 快速使用

```cpp
#include "thread_pool.hpp"

int main() {
    thread_pool pool(2);
    auto result = pool.submit([](int a, int b) { return a + b; }, 3, 4);
    return result.get() == 7 ? 0 : 1;
} // 析构时优雅关闭。
```

使用方需链接静态库目标 `thread_pool::thread_pool`；完整调用契约见 [API.md](docs/API.md)。

## 构建与运行

已验证环境：Windows / MSYS2 UCRT64、GCC 16.2.0、CMake 3.25+、Ninja，采用 C++20。项目根目录运行：

```powershell
cmake --preset ucrt64-debug -DTHREAD_POOL_BUILD_EXAMPLES=ON
cmake --build --preset ucrt64-debug
ctest --preset ucrt64-debug
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
& ./build/ucrt64-debug/thread_pool_example.exe  # 输出 7
```

## 测试与验证

Debug、Release 各 **37 项 CTest 通过**，覆盖任务恰好执行一次、两种关闭、并发关闭、负责人重入及队列锁外析构，另有头文件编译检查。验证限于上述工具链；terminate 分支、操作系统线程创建失败和硬件并发数返回 0 尚无受控测试。

## 性能摘要

Release 基准包含基础 **42 组**和扩展 **84 组**配置，每组一次预热、十次正式测量，保留原始 CSV 和环境记录。Intel Core i7-14650HX、UCRT64 GCC 16.2.0，`-O3 -DNDEBUG -std=c++20` 下，256 个任务各进行 200,000 次整数递推，8 worker 相对串行的批次耗时中位数加速比为 **6.855×**。

极短任务比直接串行更慢，该加速比只适用于记录中的机器和负载。计时边界、波动及原始数据见 [PERFORMANCE.md](docs/PERFORMANCE.md)。

## 文档入口

[API：调用契约](docs/API.md) · [DESIGN：实现依据](docs/DESIGN.md) · [PERFORMANCE：方法与数据](docs/PERFORMANCE.md) · [AI_COLLABORATION：协作与决策](docs/AI_COLLABORATION.md)

## AI 协作

本项目采用 AI 辅助开发。在开发过程中，我参与需求讨论、设计取舍与代码审查，并根据项目目标对部分 AI 实现方案提出修改意见。项目经历了多次设计调整和三轮功能精简。

AI 主要承担代码实现、测试编写与执行、性能测量以及文档整理工作。

许可证：[MIT](LICENSE)。
