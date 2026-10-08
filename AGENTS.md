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
当前已实现 Task、submit、work_thread 生命周期及 run 和 worker 身份判定；前三轮精简已完成并通过用户审核，快照 wait、任务 ID、进度登记与暂停恢复已删除。第三轮与审核后的 hpp 顺序整理一并提交到精简分支。池生命周期和关闭接口仍只有声明，后续按明确功能模块逐步实现，每轮同步进度并验证后停止。
构建包含头文件检查目标，以及使用池生命周期/状态桩的 submit 和真实 worker 行为测试；完整线程池尚不能直接使用。
worker 循环位于 src/thread_pool.cpp，构建为静态库，使用方须链接 thread_pool::thread_pool；其余已实现逻辑位于头文件。
具体进度与测试范围见 [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)。
未被用户要求实现逻辑时，仅补全声明、依赖类和注释。
