# C++20 线程池内部设计

[项目入口](../README.md) · [API 契约](API.md) · [实现与验证](IMPLEMENTATION.md) · [性能记录](PERFORMANCE.md)

本文描述当前实现的内部结构和同步规则。开发过程及精简历史见 [IMPLEMENTATION.md](IMPLEMENTATION.md) 与 [REVIEW.md](REVIEW.md)；对外调用规则见 [API.md](API.md)。

## 1. 结构与职责

线程池维护固定数量的 worker 和一个无界 FIFO 队列，使用 `std::jthread` 管理线程生命周期，使用 `std::packaged_task` / `std::future` 保存结果和原异常。

| 组成 | 职责 |
| --- | --- |
| `submit` | 头文件中的公开模板；保存 callable/参数，构造 packaged_task、future 与 Task |
| `enqueue(Task)` | 源文件中的私有入口；检查状态及任务有效性，入队后通知一个 worker |
| `Task` | 非模板外层持有 `unique_ptr<base>`；`model<R>` 持有 `packaged_task<R()>`，提供统一执行入口 |
| `work_thread::run` | 等待、领取并执行任务，根据池状态退出 |
| `stop(status_t)` | 串行完成关闭状态发布、排队任务丢弃、worker 回收及终止状态发布 |
| `worker_identity_guard` | 用 TLS 登记所属池，作用域退出时恢复原身份 |

`include/thread_pool.hpp` 保留模板、Task 和内联辅助实现；`src/thread_pool.cpp` 实现池构造/析构、关闭、enqueue 和 worker 循环，并编译进静态库。使用方须链接 `thread_pool::thread_pool`。

池和 worker 均不可复制、不可移动，保证 worker 所引用的池地址稳定。没有任务编号、进度集合、暂停状态、取消标志或独立的结果通道。

## 2. 状态与生效边界

| 状态 | 接受提交 | worker 行为 |
| --- | --- | --- |
| `RUNNING` | 是 | 队列非空时领取，否则等待 |
| `SHUTDOWN` | 否 | 排空队列，队列为空后退出 |
| `SHUTDOWN_NOW` | 否 | 不再领取，已领取任务执行结束后退出 |
| `TERMINATED` | 否 | 全部 worker 已 join，关闭完成 |

正常生命周期只有两条关闭路径：

```text
RUNNING → SHUTDOWN     → TERMINATED
RUNNING → SHUTDOWN_NOW → TERMINATED
```

两种关闭状态之间不转换，关闭后不重启。首次在队列锁内生效的关闭确定方式，后续调用等待完成。

入队、领取与关闭状态变更共用队列锁，按锁内顺序判定：成功入队即接受；领取先于立即关闭的任务继续执行，即使尚未进入 callable。FIFO 只约束领取顺序。

## 3. 锁与成员所有权

| 成员 | 同步规则 |
| --- | --- |
| `state_`、`tasks_`、`closing_thread_id_` | 由 `queue_mutex_` 保护 |
| `task_ready_` | 配合队列锁和状态/队列谓词等待；成功入队通知一个 worker，关闭通知全部 worker |
| `shutdown_mutex_` | 只串行化整个关闭流程；submit 与 worker 不使用它 |
| `worker_threads_` | 构造时创建；成功构造后仅关闭负责人回收，其他关闭调用不并发访问集合 |
| `current_worker_pool_` | 每线程独立，由身份守卫维护；用于判断是否为本池 worker |

需要同时持两把锁时，固定按 `shutdown_mutex_ → queue_mutex_` 获取。负责人重入预检查只短暂持队列锁，释放后才等待关闭锁，不能持队列锁等待关闭锁。

用户函数执行、用户捕获对象销毁和 join 均不持队列锁。立即关闭丢弃任务及 join 时仍持关闭锁。文档中的“锁外”均指队列锁外。

worker 集合最后声明，使其逆序析构时同步依赖仍然存活；退出状态和通知仍须在回收前显式完成，成员顺序本身不能唤醒线程。池析构前的外部访问协调由调用方负责。

## 4. 提交与任务所有权

`submit` 在队列锁外按值保存 callable/参数，保存的 callable 作为左值调用，保存的参数作为右值传入。返回类型推导与实际调用使用相同语义；右值引用结果由 `static_assert` 拒绝。

准备顺序为 invocation、`packaged_task<R()>`、future、Task，随后将 Task 移交给 `enqueue`。Task/model 只接管已构造的 packaged_task，不获取第二个 future。

`enqueue` 按值接管 Task，持队列锁检查 `RUNNING` 及 Task 有效性，再 `push`；成功后解锁并 `notify_one`。Task 为 noexcept 移动，队列分配失败直接传播，已有任务保持不变。参数先于局部锁构造，失败退出时先释放锁，再清理参数持有的用户对象。输入已经发生的移动不回滚。

所有权路径为：

```text
submit 准备 Task → enqueue 入队 → worker 领取 → 执行并释放
                              ↘ 立即关闭移出队列 → 丢弃并释放
```

每个有效 Task 只执行一次或直接销毁，移动所有权使领取和丢弃不会竞争同一个任务。`Task::operator()` / `execute()` 的 noexcept 依赖有效且仅调用一次的 packaged_task 不变量；内部误用不通过 catch-all 静默处理。

用户任务或结果存储异常由 packaged_task 保存，future 按原类型反馈。丢弃未执行的 packaged_task 会放弃共享状态，使 future 就绪并反馈 `broken_promise`，无需执行 callable 或制造取消异常。

future 就绪不表示 worker 已退出或用户捕获已销毁。池和 worker 释放 Task 后，调用方持有的 future 仍可能延长共享状态及捕获生命周期。

## 5. worker 循环

worker 入口创建身份守卫，在整个循环及局部 Task 清理期间保留所属池身份。循环依次执行：

1. 创建空的局部 `optional<Task>`，获取队列锁。
2. 使用谓词 `state_ != RUNNING || !tasks_.empty()` 等待，等待期间释放队列锁。
3. `SHUTDOWN_NOW` / `TERMINATED` 时退出；`SHUTDOWN` 且队列为空时退出。
4. 其余情况将队首任务移入局部 Task 并出队，随后解锁。
5. 执行任务，在队列锁外释放 Task 所有权，再进入下一轮。

任务异常不使 worker 退出。`std::jthread` 用于最终 join，不引入 stop_token 协议；退出由池状态和条件变量驱动，jthread 的 stop 请求不能替代状态变更及通知。

## 6. 构造与析构

默认数量由默认实参 `default_worker_count()` 提供，采用硬件并发数，返回 0 时保底 1；构造函数不重新计算。显式 0 抛 `std::invalid_argument`。

先初始化状态、队列和同步成员，再预留 worker 容器容量并逐个创建线程。预留容量避免启动 worker 后因 vector 扩容失败，销毁临时 worker 时等待仍处于运行状态的线程。

部分创建失败时，在队列锁内设置 `SHUTDOWN_NOW`，解锁并通知全部 worker，再在队列锁外清空 worker 集合、join 已创建线程，最后传播原异常。失败构造没有公开可用的池对象，不经过正常关闭负责人流程。

析构复用 `stop(SHUTDOWN)`，默认排空队列；显式关闭已经完成时直接返回。本池 worker 中析构本池调用 `std::terminate`；关闭负责人重入析构时，stop 抛出的错误不能逃逸 noexcept 析构，同样导致 terminate。

## 7. 关闭流程

两个公开入口先拒绝本池 worker，随后持队列锁检查 `TERMINATED`；其余情况解锁调用 `stop`，分别请求 `SHUTDOWN` 或 `SHUTDOWN_NOW`。

共用 stop 流程为：

1. 拒绝本池 worker；短暂持队列锁检查终止状态及负责人重入，随后解锁。
2. 获取关闭锁，再获取队列锁复查终止状态；前一个关闭调用可能已经完成。
3. 立即关闭先构造局部丢弃队列，再发布请求状态和负责人 ID，并交换队列所有权。准备分配失败发生在状态改变之前，本次调用不改变池状态、队列或负责人。
4. 释放队列锁并通知全部 worker。立即关闭销毁局部丢弃队列，再清空 worker 集合并 join；优雅关闭由 worker 排空队列后退出。此阶段持续持有关闭锁。
5. 持队列锁发布 `TERMINATED` 并清空负责人 ID，随后释放关闭锁。

其他关闭调用者等待关闭锁时不持队列锁，取得锁后复查已终止状态并返回，不重复回收，也不升级或恢复任务。终止状态早检可能在负责人释放关闭锁前返回，此时丢弃反馈和 join 已完成。

关闭不要求先读取 future，也不汇总任务异常。立即关闭不会中断已领取任务；任务不结束时，两种关闭都可能持续阻塞。

## 8. 自等待与用户回调

本池 worker 调用任一关闭接口，在改变状态和等待前抛 `std::logic_error`。TLS 只匹配所属池，其他池的 worker 不会被直接拒绝。

`closing_thread_id_` 由队列锁保护，在关闭生效时登记，丢弃、join 和完成发布结束后清空。负责人重入检查必须在获取非递归关闭锁之前完成，避免同一线程再次等待自己持有的锁。

捕获对象在关闭线程丢弃任务时同步重入关闭，会得到 logic_error；在关闭期间 submit 会被拒绝。用户析构须自行处理这些异常，避免异常逃逸 noexcept 析构。

上述检测只处理本池 worker 自关闭和同线程负责人重入。跨池、跨线程等待环，以及全部 worker 等待本池排队任务的 future，仍由调用方避免；不引入协作等待或任务窃取。

行为验证及尚未触发的 terminate、操作系统线程创建失败、硬件并发数返回 0 路径见 [验证边界](IMPLEMENTATION.md)。性能基准、基础 42 组和扩展 84 组的本机实测见 [PERFORMANCE.md](PERFORMANCE.md)。
