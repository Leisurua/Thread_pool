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
构建包含头文件检查、基础任务执行 5 项、优雅关闭 6 项、立即关闭 6 项、关闭竞争与自等待 6 项、生命周期与异常安全 12 项及 submit、真实 worker 回归，共 37 项 CTest。基础、优雅关闭、立即关闭三组每项验证单 worker 和 4 worker 且只用公共接口；关闭竞争组通过公共接口关闭，以只读友元观察状态/负责人，worker 集合只在全部关闭调用完成后读取；生命周期组覆盖指定/默认数量、显式 0 拒绝、部分创建失败回收、析构排空、关闭后不重复回收、入队分配失败、锁外销毁捕获，以及 worker 自关闭、并发关闭无升级、负责人重入、关闭准备失败回滚。关闭分组以 TLS 析构计数确认 worker 退出，立即关闭领取竞态共 200 轮、51,200 个任务。生命周期/stop 定义桩已删除，可直接链接使用完整公共接口；submit 测试仍用友元状态夹具同步驱动 Task。
公开 submit 在头文件中准备 Task/future；私有 enqueue、池构造/析构、shutdown/shutdown_now/stop 与 worker 循环在 src/thread_pool.cpp 实现，构建为静态库，使用方须链接 thread_pool::thread_pool。其余已实现逻辑位于头文件。
具体进度与测试范围见 [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md)。
性能验证使用 ucrt64-release（-O3 -DNDEBUG），该预设启用独立 thread_pool_benchmark；构建后运行 `& ./benchmarks/run.ps1 -Rounds 10`。42 组各至少 10 次，另有预热；保存原始 CSV、环境/编译命令/源码哈希和中位数/波动，详见 docs/PERFORMANCE.md。禁止将 Debug 数据或完整批次耗时当作纯提交开销。基准不加入 CTest，产品不增加统计接口。submit/lifecycle 测试替换 new/delete 使用 noinline，以兼容 Release 警告检查。
未被用户要求实现逻辑时，仅补全声明、依赖类和注释。
扩展性能组使用 `& ./benchmarks/run.ps1 -Suite advanced -Rounds 10`，84 组各至少 10 次：固定总递推次数的粒度扫描、受控 worker 的批量入队、积压任务立即关闭；单独保存反馈观察及 future 异常读取/释放时间。丢弃反馈含观察遍历，关闭总耗时含门控等待，不能描述为裸锁或纯销毁耗时。
