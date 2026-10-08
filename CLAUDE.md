# 本机开发环境与约定

本文件供后续会话查阅工具链、构建和编辑器配置。项目进度见 [IMPLEMENTATION.md](docs/IMPLEMENTATION.md)，调用契约见 [API.md](docs/API.md)。

## 本机目标环境

| 项目 | 配置 |
| --- | --- |
| 平台 | Windows / MSYS2 UCRT64 |
| 编译器 | GNU GCC 16.2.0（`C:/msys64/ucrt64/bin/g++.exe`） |
| 构建工具 | CMake（项目最低要求 3.25）+ Ninja（`C:/msys64/ucrt64/bin/ninja.exe`） |
| 语言标准 | C++20，禁用 GNU 扩展 |
| 默认构建 | Debug，预设 `ucrt64-debug` |
| 备用构建 | Release，预设 `ucrt64-release` |

预设为构建进程前置 UCRT64 工具目录，不修改系统 PATH；不要求调用方预先配置全局工具链路径。CMake 本身需能从命令行调用。

## 构建与验证

全部命令在项目根目录执行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --test-dir build/ucrt64-debug --output-on-failure
```

构建目录固定为 `build/ucrt64-debug/`，生成 `compile_commands.json`。头文件检查目标验证独立编译和重复包含；submit 测试使用池生命周期桩，worker 测试通过池生命周期/状态桩驱动真实产品 worker。完整池生命周期及关闭接口尚未实现；快照 wait、任务进度登记和暂停恢复已删除。前三轮已通过用户审核，第三轮与 hpp 顺序整理一并提交，之后逐步实现未完成功能。

单独调用 ctest 或测试可执行文件时，它们继承当前终端环境，不会自动继承 configure/build 预设中的 PATH。若终端尚未包含 UCRT64 运行库目录，可在当前 PowerShell 会话中执行，再运行测试：

```powershell
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
```

该设置只影响当前会话及其子进程，不修改系统环境。

## clangd

项目 [.clangd](.clangd) 从 `build/ucrt64-debug/` 读取编译数据库，并按 UCRT64 GCC / C++20 解析。

编辑器使用 `C:/msys64/ucrt64/bin/clangd.exe`，启动参数需包含：

```text
--query-driver=C:/msys64/ucrt64/bin/g++.exe
```

本机 VS Code 已设置该目录的 query-driver 通配符。

## 开发约定

- 通过 `CMAKE_CXX_STANDARD 20`、`CMAKE_CXX_STANDARD_REQUIRED ON` 和 `CMAKE_CXX_EXTENSIONS OFF` 明确语言标准，使用 `-std=c++20`；保留目标的 `cxx_std_20` 要求。
- 警告由 `thread_pool` 静态库目标通过 PUBLIC 选项统一施加，本机默认启用 `-Wall -Wextra -Wpedantic -Werror`；修改策略集中在 CMakeLists.txt。
- 不通过增加 `-Wno-*` 消除告警，先确认并修复真实缺陷。
- 构建目录固定在已忽略的 `build/`，不引入另一套 `cmake-build-*` 目录。
- 当前 worker 的 run 位于 `src/thread_pool.cpp`，CMake 将其编译为静态库；其余已实现逻辑仍在头文件，使用方须链接 `thread_pool::thread_pool`。
- 未被用户要求实现逻辑时，仅补全声明、依赖类和注释。
