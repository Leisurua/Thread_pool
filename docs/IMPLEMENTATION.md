# 实现进度与验证

[项目入口](../README.md) · [API 契约](API.md) · [内部设计](DESIGN.md)

代码采用 C++20 头文件与源文件分离结构，worker 的 run 在源文件中实现，其余已实现逻辑位于头文件，身份守卫完整实现在 thread_pool 的私有嵌套类内部。当前完成 Task、submit、worker 循环和进度登记，完整线程池尚不能直接使用。本文列出代码进度和检查范围；调用契约以 [API.md](API.md) 为准，同步与内部结构以 [DESIGN.md](DESIGN.md) 为准。

## 1. 当前实现清单

| 模块 | 进度 | 说明 |
| --- | --- | --- |
| 状态与接口声明 | 已声明 | 五种状态、固定 worker、暂停恢复、关闭及 wait |
| Task 类型擦除 | 已实现 | 非模板外层、base/model、packaged_task 执行与取消 |
| submit | 已实现 | 锁外准备、锁内状态检查与编号入队、失败回滚、future 返回 |
| 结果与提交异常 | 已实现 | submission_rejected、submission_error、task_cancelled、task_execution_error |
| thread_pool_call_error | 仅声明 | 类型及基类已确定，构造函数待实现 |
| thread_pool 构造与析构 | 仅声明 | 默认 worker 数量函数也待实现 |
| work_thread 生命周期及 run | 已实现 | jthread 启动、状态谓词等待、FIFO 领取、锁外执行及退出；池须先通知退出 |
| worker 身份判定 | 已实现 | run 的线程局部 RAII 守卫与 is_current_worker；wait/关闭接口拒绝检查待接入 |
| pause / resume | 仅声明 | 状态变更待实现 |
| shutdown / shutdown_now | 仅声明 | 共用关闭、升级、取消反馈与 join 待实现 |
| finish_task | 已实现 | 有序 set 删除 ID、最早丢弃记录、锁外通知进度 |
| wait | 仅声明 | 单一有效快照等待及 worker 入口拒绝检查待实现 |

`src/thread_pool.cpp` 实现 work_thread::run，编译进 thread_pool 静态库；使用方须链接该库。测试中的生命周期桩不属于产品实现。

## 2. 后续实现要求

以下规则已确定，尚需落地并验证：

- 构造采用硬件并发数，返回 0 时采用 1；显式传入 0 抛 invalid_argument。成功后为 RUNNING，部分创建失败回收已创建线程。
- pause 暂停领取，resume 恢复领取；关闭后两者抛 thread_pool_call_error。
- 优雅关闭继续处理队列；立即关闭丢弃未领取任务，锁外完成取消反馈。并发关闭共用一次 join，允许优雅关闭升级。
- wait 取得固定接受快照，同一时刻只允许一个有效等待；历史丢弃计入 cancelled，后续提交不扩展范围。
- 本池 worker 调用 wait 或关闭接口时，在等待和状态变更前拒绝；本池 worker 内析构本池调用 terminate。
- callable、取消反馈、用户对象销毁及 join 在主锁外执行；等待期间释放主锁。
- 析构前由调用方协调所有外部访问，成员销毁顺序保证 worker 退出时同步依赖仍存活。

worker 内部状态分支已有真实线程测试，但上述池生命周期和公共接口仍未实现；测试桩直接驱动状态不代表公共 pause/resume、关闭或 wait 已完成。

## 3. 构建目标

| 目标 | 作用 |
| --- | --- |
| `thread_pool` / `thread_pool::thread_pool` | STATIC 库，编译 worker 循环，并传递包含目录、C++20、线程依赖和警告选项 |
| `thread_pool_header_check` | OBJECT 目标，验证头文件独立编译与重复包含，不链接完整线程池 |
| `thread_pool_submit_test` | 使用测试桩验证产品 submit 与 Task；BUILD_TESTING 开启时构建 |
| `thread_pool_worker_test` | 使用池生命周期及状态测试桩，验证真实产品 worker/run/finish_task；BUILD_TESTING 开启时构建 |

CMake 使用 `include(CTest)`，默认启用 BUILD_TESTING；可通过 `-DBUILD_TESTING=OFF` 关闭行为测试目标。CTest 测试名称为 `thread_pool_submit` 与 `thread_pool_worker`，各自超时为 30 秒。

两个测试可执行文件分别定义自己的池生命周期桩，不应将这些桩合入库或合并链接为同一个程序。静态库能构建通过，也不代表仍只有声明的池生命周期和公共接口已可链接使用。

## 4. 编译与测试

在项目根目录执行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --test-dir build/ucrt64-debug --output-on-failure
```

产物位于 `build/ucrt64-debug/`。工具链、警告策略和 clangd 设置见 [CLAUDE.md](../CLAUDE.md)。

## 5. 已有测试覆盖

[submit_test.cpp](../tests/submit_test.cpp) 使用测试专用构造、析构桩和友元同步驱动，不创建真实 worker。测试直接调用产品 submit、Task 与 finish_task，覆盖：

- 返回 future 时任务尚未执行，submit 不等待执行结束。
- 值、void、左值引用结果，以及 move-only callable / 参数、std::ref 和仅左值可调用对象。
- 原函数异常包装、原异常查询，以及 packaged_task 内部结果存储异常保持原类型。
- 取消使 future 就绪，原函数不被调用。
- PAUSED 接受提交，三种关闭状态拒绝且不推进接受边界。
- 用户对象复制时抛 submission_rejected，仍包装为 submission_error。
- 锁外准备、按成功入队顺序编号，以及多线程提交的编号和未完成记录一致性。
- 队列分配失败后的回滚与重试，以及任务编号溢出的异常反馈。

[worker_test.cpp](../tests/worker_test.cpp) 使用池生命周期桩和友元状态驱动，创建真实产品 work_thread，覆盖：

- worker 能识别本池身份，池外线程和其他池不会被误判；任务内提交到本池可继续执行。
- 原函数异常经 future 反馈，后续任务继续执行。
- 暂停时不领取，恢复后单 worker 按 FIFO 执行；暂停时已入队任务仍有 ID 和未完成记录。
- 双 worker 乱序完成时保留最早未完成 ID，完成登记发出进度通知。
- worker 持有最后一个共享状态引用时，释放 Task 所触发的用户析构可以提交到本池，验证销毁在锁外且先于完成登记。
- 优雅关闭状态解除暂停并排空队列；立即关闭状态保留已领取任务，不领取剩余任务。
- 关闭及 TERMINATED 状态使空闲 worker 退出；取消反馈后删除 ID，逆序取消仍记录最早丢弃 ID。

测试通过不代表公共 pause/resume、wait 的完整快照算法及拒绝检查、关闭升级、构造失败清理或产品析构行为已通过验证。

future 就绪先于 worker 的完成登记；测试需要确认池内进度时，通过友元驱动等待 unfinished_tasks_ 更新，不以 future.get() 返回代替进度检查。产品 wait 仍待实现。
