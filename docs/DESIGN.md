# 内部设计

[项目入口](../README.md) · [API 契约](API.md)

本文说明内部怎样保证任务所有权、关闭同步和生命周期安全；调用行为见 API，不另建取消、暂停或进度协议。

## Task 与提交职责

公开 `submit` 在头文件中准备 invocation、`packaged_task<R()>`、future 和 Task，再交给源文件中的私有 `enqueue(Task)`。返回类型推导与实际调用一致：decay 后的 callable 以左值调用，tuple 保存的参数通过 apply/invoke 以右值传入；右值引用结果由 static_assert 拒绝。

Task 的非模板外层持有 `unique_ptr<base>`，`model<R>` 接管已构造的 packaged_task，统一执行入口返回 void。结果及任务原异常沿用标准库共享状态，不获取第二个 future，也不包装异常。

Task 禁止复制，移动 noexcept。`enqueue` 按值接管 Task，在队列锁内检查状态和有效性，再 push；成功后解锁并 notify_one。对象准备在锁外，Task 参数先于局部锁构造，失败展开时先释放锁，再销毁持有用户捕获的参数。队列分配失败不会改变已有任务。

```text
submit 准备 Task → enqueue 入队 → worker 领取 → 执行并释放
                              ↘ 立即关闭移出队列 → 丢弃并释放
```

移动所有权使领取与丢弃不竞争同一个 Task。统一执行入口的 noexcept 依赖有效且仅执行一次的 packaged_task 不变量；用户执行异常由 packaged_task 保存，不用 catch-all 掩盖内部误用。future 可能延长共享状态和捕获生命周期，因此释放 Task 不等于所有捕获已析构。

## 状态与双锁分工

| 成员 | 保护方式与职责 |
| --- | --- |
| state_、tasks_、closing_thread_id_ | queue_mutex_ 保护，统一入队、领取与状态变更顺序 |
| task_ready_ | 配合队列锁和状态/队列谓词等待；入队通知一个 worker，关闭通知全部 |
| shutdown_mutex_ | 串行化整个关闭与回收；submit、worker 不使用它 |
| worker_threads_ | 构造时创建；成功构造后仅关闭负责人回收 |
| current_worker_pool_ | 每线程独立的 TLS，私有身份守卫维护 |

正常状态路径为 `RUNNING → SHUTDOWN → TERMINATED` 或 `RUNNING → SHUTDOWN_NOW → TERMINATED`，两种关闭状态不相互转换。状态与队列共用锁，成功 push、队首出队和首次关闭状态发布分别确定接受、领取和关闭生效边界。

需要同时持锁时固定按 **shutdown_mutex_ → queue_mutex_** 获取。负责人重入预检查只短暂持队列锁，释放后才等待关闭锁。执行用户函数、销毁捕获和 join 不持队列锁；关闭线程清理和 join 时仍持关闭锁，本文“锁外”指队列锁外。

## worker 与 jthread

`work_thread::run()` 入口创建私有 `worker_identity_guard`，保存原 TLS 指针并登记所属池，离开作用域时恢复；身份覆盖整个循环及局部 Task 清理。

每轮先创建局部 `optional<Task>`，持队列锁以 `state_ != RUNNING || !tasks_.empty()` 等待。SHUTDOWN_NOW / TERMINATED 时退出，SHUTDOWN 且队列为空时退出；其余情况移动队首 Task 并出队，解锁后执行和释放，再进入下一轮。领取先于立即关闭的任务不会被撤回。

选择 `std::jthread` 让线程回收随 worker 的 RAII 生命周期完成，降低正常析构及部分构造失败时遗漏 join 的风险。当前没有 stop_token 取消协议；jthread 析构中的 stop 请求不能替代池状态变更与条件变量通知，也不会强制中断用户任务。

## 构造与回收

默认 worker 数量由默认实参 `default_worker_count()` 提供，构造函数不重新计算。先初始化状态、队列和同步成员，再 reserve worker 容器并逐个创建线程，避免启动 worker 后因 vector 扩容失败而回收仍在等待的临时 worker。

创建部分失败时，先持队列锁设置 SHUTDOWN_NOW，解锁通知全部 worker，再锁外清空集合、join 已创建线程，最后传播原异常。失败构造不经过正常关闭负责人流程。

池和 worker 均不可复制、不可移动，保持池地址稳定；worker 集合最后声明，使逆序析构时同步依赖仍存活。成员顺序本身不能唤醒线程，回收前仍须显式设置退出状态并通知。

析构复用 `stop(SHUTDOWN)`；本池 worker 析构本池直接 terminate，负责人重入析构时 stop 抛出的异常不能逃逸 noexcept 析构，也导致 terminate。

## 关闭流程

公开入口拒绝本池 worker、检查已终止状态，然后将请求交给共用 `stop(status_t)`；入口不改变关闭方式。

1. stop 拒绝本池 worker，短暂持队列锁检查 TERMINATED 和负责人同线程重入，再释放队列锁。
2. 获取关闭锁，再持队列锁复查 TERMINATED；前一个关闭调用可能已经完成。
3. 立即关闭先构造局部丢弃队列，再发布请求状态、负责人 ID 并交换队列所有权；优雅关闭保留队列由 worker 排空。
4. 释放队列锁、通知全部 worker，立即关闭销毁丢弃队列，再清空 worker 集合并 join；整个阶段持续持关闭锁。
5. 持队列锁发布 TERMINATED、清空负责人 ID，最后释放关闭锁。

其他关闭调用取得关闭锁后复查 TERMINATED，不重复回收，也不升级方式。终止状态早检可能在负责人释放关闭锁前返回，此时丢弃反馈与 join 已完成。

局部丢弃队列准备发生在状态修改之前；若构造抛异常，状态、队列和负责人未改变，原异常传播，两锁由 RAII 释放。本机 UCRT64 空队列构造发生分配，`shutdown_preparation_failure` 通过替换 new 注入 bad_alloc，检查仍为 RUNNING、负责人未登记、已有任务和后续提交/关闭正常。空队列是否分配依赖标准库，不能宣称其他实现拥有相同触发点，也不把此保证扩展为整个关闭流程的任意失败回滚。

## 自等待与重入保护

TLS 指针只匹配本池 worker。另一条路径来自池外负责人丢弃 Task：用户捕获析构可能再次关闭本池，TLS 无法识别，因此用队列锁保护的 closing_thread_id_ 登记负责人，从首次关闭生效覆盖到清理、join 和完成发布。

负责人预检查必须在获取非递归关闭锁之前完成，否则同线程会等待自己持有的锁；检查后释放队列锁，避免反向锁顺序。该机制只覆盖两类同线程自等待，不引入协作等待或任务窃取来处理一般等待环。

实现位置：[头文件](../include/thread_pool.hpp) 保存模板、Task 和身份守卫，[源文件](../src/thread_pool.cpp) 实现生命周期、enqueue、关闭和 worker 循环。验证范围见 [README](../README.md#测试与验证)，设计取舍过程见 [AI_COLLABORATION.md](AI_COLLABORATION.md)。
