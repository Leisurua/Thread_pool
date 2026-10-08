# C++20 固定 worker 数量线程池

V1.0 目标是实现一个固定 worker 数量、使用 FIFO 队列的 C++20 线程池，提供任务提交、future 结果反馈及两种关闭方式。前三轮精简已完成并通过用户审核：异常反馈、快照进度移除及暂停恢复移除。

## 当前进度

已实现 `Task` 类型擦除、`std::packaged_task` 原异常反馈、`submit()`、work_thread 生命周期与 worker 循环、worker 身份判定，并配置头文件编译检查、submit 与真实 worker 行为测试。任务 ID、快照等待与 set 进度登记已删除；未执行 Task 销毁通过 future 反馈 `broken_promise`。

线程池构造、析构、`shutdown()` 和 `shutdown_now()` 仍只有声明，完整线程池尚不能直接使用。暂停恢复接口与状态已删除。测试通过专用池生命周期及状态桩驱动产品逻辑，不代表公共关闭流程已完成。前三轮审核记录见 [REVIEW.md](docs/REVIEW.md)，后续逐步实现未完成功能。

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
├── include/thread_pool.hpp   # 接口声明及 Task、submit、worker 生命周期实现
├── src/thread_pool.cpp       # worker 的 run 实现，编译为静态库
├── tests/submit_test.cpp     # 使用生命周期桩的 submit 行为测试
├── tests/worker_test.cpp     # 使用池生命周期/状态桩的真实 worker 行为测试
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
