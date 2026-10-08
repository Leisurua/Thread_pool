# 本目录开发环境

- 平台：本机 Windows / MSYS2 UCRT64。
- 编译器：GNU GCC 16.2.0，路径 `C:/msys64/ucrt64/bin/g++.exe`。
- 标准：C++20，禁用 GNU 扩展（`-std=c++20`）。
- 构建工具：CMake + Ninja（`C:/msys64/ucrt64/bin/ninja.exe`）。
- 默认构建类型：Debug，预设 `ucrt64-debug`。
- 在项目根目录运行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --test-dir build/ucrt64-debug --output-on-failure
```

构建产物位于 `build/ucrt64-debug/`；预设仅为本项目设置 PATH，不修改系统环境。
项目 `.clangd` 从该构建目录读取 `compile_commands.json`，并按 UCRT64 GCC / C++20 解析。
编辑器使用 `C:/msys64/ucrt64/bin/clangd.exe`，启动参数需包含
`--query-driver=C:/msys64/ucrt64/bin/g++.exe`（本机 VS Code 已设置该目录通配符）。
当前已实现 Task、submit/enqueue、work_thread 生命周期与 run、worker 身份判定、池构造、优雅析构及 shutdown/shutdown_now/stop；前三轮精简已通过用户审核。默认 worker 数量沿用 default_worker_count()。queue_mutex_ 维护原有任务入队/领取及状态变更顺序；shutdown_mutex_ 只互斥并发关闭调用，submit 和 worker 不使用它；同时持锁时按关闭锁到队列锁的顺序获取。获取关闭锁前先在队列锁下检查负责人重入，检查后释放队列锁；丢弃和 join 时不持队列锁。每轮同步进度并验证后停止。
构建包含头文件检查及 submit、真实 worker、生命周期三项测试。生命周期/stop 定义桩已删除，可直接链接使用完整公共接口；submit 测试仍用友元状态夹具同步驱动 Task。
公开 submit 在头文件中准备 Task/future；私有 enqueue、池构造/析构、shutdown/shutdown_now/stop 与 worker 循环在 src/thread_pool.cpp 实现，构建为静态库，使用方须链接 thread_pool::thread_pool。其余已实现逻辑位于头文件。
具体进度与测试范围见 [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)。
未被用户要求实现逻辑时，仅补全声明、依赖类和注释。
