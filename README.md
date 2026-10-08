# C++20 固定 worker 数量线程池

V1.0 目标是实现一个固定 worker 数量、使用 FIFO 队列的 C++20 线程池，提供任务提交、future 结果反馈及两种关闭方式。前三轮精简已完成并通过用户审核：异常反馈、快照进度移除及暂停恢复移除。

## 当前进度

V1.0 核心功能开发已完成（模块 1 至 4），性能验证（模块 5）尚未开始；按包含可复现性能数据的项目完成标准，整体尚未全部完成。

已实现 `Task` 类型擦除、`std::packaged_task` 原异常反馈、`submit()`、work_thread 生命周期与 worker 循环、worker 身份判定，并配置头文件编译检查及 submit、真实 worker、生命周期三项行为测试。任务 ID、快照等待与 set 进度登记已删除；未执行 Task 销毁通过 future 反馈 `broken_promise`。

线程池构造、优雅析构及两种显式关闭均已实现，可直接链接使用。默认数量沿用 `default_worker_count()`，部分 worker 创建失败时通知并回收已创建线程。`shutdown()`、`shutdown_now()` 与共用 `stop()` 位于 `src/thread_pool.cpp`；关闭锁 `shutdown_mutex_` 串行化整个关闭，队列锁 `queue_mutex_` 保护状态及任务队列；其他关闭调用者等待关闭锁，不升级方式。立即关闭在队列锁外销毁未领取任务，所有关闭均等待 worker 退出。三个测试已删除生命周期/stop 定义桩。前三轮审核记录见 [REVIEW.md](docs/REVIEW.md)。

V1.0 不包含有界队列、动态 worker 数量或 work-stealing。FIFO 保证任务领取顺序，不保证多个 worker 实际进入 callable 或完成任务的顺序。

## 构建与检查

本机目标环境为 Windows / MSYS2 UCRT64，使用 GNU GCC 16.2.0、CMake 和 Ninja。语言标准为 C++20，禁用 GNU 扩展；默认构建类型为 Debug。

在项目根目录执行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --test-dir build/ucrt64-debug --output-on-failure
```

预设指定 `C:/msys64/ucrt64/bin/g++.exe` 和 `C:/msys64/ucrt64/bin/ninja.exe`，并检查 GCC 版本。构建产物位于 `build/ucrt64-debug/`，预设仅为构建进程设置 PATH。备用 Release 预设为 `ucrt64-release`。

当前构建为静态库，使用方须链接 CMake 目标 `thread_pool::thread_pool`，包含头文件本身不足以链接 worker 实现。独立运行测试时的 UCRT64 运行库环境见 [本机开发说明](CLAUDE.md)。

## 项目结构

```text
thread_pool_project/
├── include/thread_pool.hpp   # 接口声明及 Task、submit 准备任务、worker 生命周期实现
├── src/thread_pool.cpp       # 池构造/析构、关闭流程、私有 enqueue 及 worker 的 run，编译为静态库
├── tests/submit_test.cpp     # 回收 worker 后同步驱动 submit 的行为测试
├── tests/worker_test.cpp     # 使用产品关闭接口的真实 worker 行为测试
├── tests/lifecycle_test.cpp  # 产品生命周期、并发关闭、重入及失败回收测试
├── docs/
│   ├── API.md                # 调用契约与异常接口
│   ├── DESIGN.md             # 状态、同步和内部任务结构
│   └── IMPLEMENTATION.md     # 实现进度、后续要求和验证范围
├── CMakeLists.txt
├── CMakePresets.json
├── AGENTS.md                 # 开发代理约定
└── CLAUDE.md                 # 本机构建与编辑器说明
```

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [API.md](docs/API.md) | 构造、提交、关闭、future 与异常反馈规则 |
| [DESIGN.md](docs/DESIGN.md) | 状态转换、并发边界、类型擦除、提交流程与模块精简 |
| [IMPLEMENTATION.md](docs/IMPLEMENTATION.md) | 已实现和待实现清单、构建目标及测试边界 |
| [CLAUDE.md](CLAUDE.md) | 工具链、clangd 配置及构建约定 |

文档中的“待实现”表示规则已确定、代码尚未完成；“待确认”表示仍需作出设计决定。具体实现状态以代码和 [实现进度](docs/IMPLEMENTATION.md) 为准。
