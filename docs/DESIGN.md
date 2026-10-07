# 线程池内部设计

[项目入口](../README.md) · [API 契约](API.md) · [实现进度](IMPLEMENTATION.md)

本文记录 V1.0 的内部结构与同步规则。Task、submit、worker 循环和进度登记已实现；池生命周期、pause/resume、关闭及 wait 的内容是后续实现规格。调用方行为集中在 [API.md](API.md)。

## 1. 范围与状态

线程池采用固定 worker 集合和 FIFO 任务队列。FIFO 按主锁内成功领取任务的顺序保证，不约束多个 worker 实际进入 callable 或完成的顺序。V1.0 不包含有界队列、动态 worker 或 work-stealing。

`status_t` 同时表达运行状态和关闭方式；枚举整数值不表达关闭优先级。

| 状态 | 含义 | 新任务提交 | 尚未领取的任务 |
| --- | --- | --- | --- |
| `RUNNING` | 正在运行 | 接受 | 允许领取 |
| `PAUSED` | 已暂停，已有任务可能仍在执行 | 接受 | 保留队列，暂停领取 |
| `SHUTDOWN` | 优雅关闭中 | 拒绝 | 继续领取，可升级为立即关闭 |
| `SHUTDOWN_NOW` | 立即关闭中 | 拒绝 | 丢弃，等待已领取任务结束 |
| `TERMINATED` | 已关闭，所有 worker 已退出 | 拒绝 | 关闭处理完成 |

构造成功后进入 RUNNING；RUNNING 与 PAUSED 可相互转换。两者均可进入任一种关闭状态；SHUTDOWN 只可升级为 SHUTDOWN_NOW。关闭最终进入 TERMINATED，不可降级或重新运行。

worker 使用 `std::jthread`。池与 worker 均禁用复制和移动，二者互为友元；worker 通过非拥有引用访问池，由池协调其生命周期。

## 2. 并发边界与主互斥锁

成功入队即接受；worker 成功领取并出队即正在执行。已领取任务承诺继续执行，没有额外启动确认阶段。

| 并发先后 | 结果 |
| --- | --- |
| 成功入队先于关闭生效 | 已接受，按最终关闭方式执行或丢弃 |
| 关闭生效先于入队 | 同步拒绝提交，不返回 future |
| 领取先于暂停生效 | 已领取任务继续执行 |
| 领取先于立即关闭或升级 | 已领取任务继续执行，关闭等待它结束 |
| 立即关闭或升级先于领取 | 尚未领取任务被丢弃 |

相关先后以同一主互斥锁内的生效顺序判定，不能用调用开始时刻代替。已领取任务即使尚未进入 callable，也不会撤回。

`mutex_` 统一保护：

- 状态检查、暂停恢复和关闭方式升级。
- 队列入队、worker 领取及立即关闭时移出待执行任务。
- 接受编号、未完成任务记录、丢弃记录与 wait 快照边界。
- 单一有效 wait 和共用 join 的资格。

callable 执行、取消结果反馈、用户对象销毁与 worker join 在主锁外完成。条件变量等待释放主锁，唤醒后重新持锁检查条件；阻塞关闭和 wait 不能持续持锁等待其他线程推进。

## 3. Task 的类型擦除（已实现）

`Task` 是非模板类，直接存入 `std::queue<Task>`，不需要独立的 queued_task。外层保存任务编号及 `std::unique_ptr<base>`，禁用复制，支持 noexcept 移动。

`base` 提供虚析构、execute 和 cancel。具体 `model<R>` 接管 submit 已构造的 `std::packaged_task<R()>` 和共享取消标志；模型不构造原函数或参数，不调用 get_future，也不另建结果共享状态。

packaged_task 的调用对象按值保存原函数、参数 tuple 和取消标志。执行时先检查取消标志，再通过 `std::apply` 展开移动后的 tuple，以 `std::invoke` 调用保存的函数。函数作为左值调用，保存的参数作为右值传入。

`Task::operator()()` 返回 void，原任务结果由 packaged_task 保存。取消时设置共享标志，再调用同一个 packaged_task，将 `task_cancelled` 存入共享状态，不调用原函数。

池必须保证执行与取消二选一、只调用一次、彼此不并发，且不对已移动的 Task 调用。取消标志因此无需原子类型。Task 不直接保存 promise/future，不提供 run；`work_thread::run()` 才是 worker 循环。

原函数异常包装为 `task_execution_error`；packaged_task 在标准库内部存储结果时发生的异常保持原类型。两者均通过 future 反馈，异常接口见 [API.md](API.md#4-future-与异常反馈)。

## 4. submit 的准备、入队和返回（已实现）

返回类型按保存后的函数与参数的实际调用方式推导，支持值、void 和左值引用，不支持右值引用。

主锁外的准备流程：

```text
保存 callable、参数 tuple 和共享取消标志
  → 构造 packaged_task<R()>
  → get_future() 取得 future<R>
  → packaged_task 移入 Task::model<R>，Task 编号暂为 0
```

移动 packaged_task 后，它与 future 仍关联同一个标准库共享状态；模型不捕获自身 this，不另建 promise。共享状态的生命周期由 packaged_task 和 future 共同维护。

主锁内的接受流程：

1. 检查状态，只接受 RUNNING / PAUSED。
2. 检查编号溢出，计算下一个接受编号并写入 Task。
3. 将编号登记到 `unfinished_tasks_`。
4. 将 Task 入队；失败则回滚本次未完成记录。
5. 只有入队成功才更新 `last_accepted_id_`。

成功后释放主锁，通知 `task_ready_` 并返回 future。接受编号按入队顺序分配，不按 submit 开始顺序或准备完成顺序分配。任务可能在 submit 返回前完成。

准备、加锁和入队失败分阶段包装为 `submission_error`；状态拒绝独立抛出 `submission_rejected`。用户对象构造时即使抛出 submission_rejected，也不能误判为池拒绝。锁在 Task 之后构造，异常清理先解锁，再销毁用户对象。

## 5. worker 循环（已实现）

work_thread::run 在 `src/thread_pool.cpp` 中定义，头文件保留声明；构建为静态库供使用方链接。

work_thread 构造时启动 jthread。run 入口通过池的私有嵌套类 worker_identity_guard 登记线程局部的所属池，退出时由 RAII 恢复原值；守卫禁止复制和移动，仅保存旧池指针。is_current_worker 比较私有 thread_local 指针 current_worker_pool_ 与当前池地址，不会将其他池的 worker 误判为本池 worker。这些机制不提供公共 API，wait 与关闭接口的入口拒绝检查仍待接入。

每轮先持主锁进行条件变量等待。谓词为“进入任一关闭状态，或处于 RUNNING 且队列非空”；PAUSED 即使队列非空也不能领取。唤醒后重新持锁检查：SHUTDOWN_NOW / TERMINATED 退出，SHUTDOWN 在队列为空时退出，否则继续领取。优雅关闭因此不受暂停限制。

领取时将队首 Task 移至局部对象并 pop，随后解锁、调用 Task、在锁外释放局部 Task，再通过 finish_task 登记进度。run 不重新分配 ID，领取后不因暂停或立即关闭撤回执行。用户函数异常经 future 反馈，不中断循环。立即关闭的排队任务由关闭流程负责取消，worker 不参与取消队列。

packaged_task 调用结束时 future 已就绪，但此时 Task 释放和 finish_task 登记可能尚未完成。Task 释放仅保证 worker 不再持有该模型；future 仍可能延长共享状态和其中对象的生命周期。

work_thread 的析构依赖池先设置退出状态并通知 worker，再由 jthread 等待线程退出；jthread 的 stop 请求不替代池状态协议。池的构造失败回收与公共关闭流程仍待实现。

## 6. 进度记录（已实现）与 wait（待实现）

`last_accepted_id_` 表示已成功接受的任务边界。`unfinished_tasks_` 采用 `std::set<task_id_type>`，包含排队和已领取但尚未处理结束的任务；submit 在成功入队前登记 ID，入队失败时按 ID 回滚。完成或取消结果反馈结束后由 finish_task 删除对应 ID。乱序完成不能替代较早任务完成。

有序 set 的 begin 提供最小未完成 ID，按 ID 删除为 O(log n)。wait 在主锁内占用单一有效等待资格，并保存当前接受边界。set 为空，或最小未完成编号超过快照边界时，快照全部处理结束。边界后提交的任务不扩展等待范围。

`first_cancelled_id_` 记录最早丢弃编号，0 表示尚无丢弃。该编号不超过快照边界时，结果为 cancelled；否则为 completed。因此历史丢弃仍计入后续包含它的快照，快照外丢弃不影响本次结果。发现丢弃不提前返回。

未完成 set 只随未完成任务增长，不积累所有已完成编号；丢弃历史通过最早编号记录。finish_task 在主锁内删除 ID 并按需要更新最早丢弃编号，解锁后通知 `progress_changed_`。

暂停、关闭中及关闭后均允许池外 wait；暂停可能阻止进度，需另一线程恢复或关闭。wait 的结果不要求 worker 已退出。

## 7. 暂停、关闭与生命周期（待实现）

pause 只阻止新的领取，resume 恢复领取；重复调用保持状态。开始关闭后，pause/resume 抛出调用错误，不改变关闭方式或进度。

优雅关闭解除暂停对领取的限制，继续处理队列。立即关闭在主锁内移出尚未领取任务，在锁外完成取消反馈及用户对象销毁，随后记录处理结束；已领取任务继续执行。

并发关闭共用一次 worker join，通过 `join_in_progress_` 协调 join 资格，其他调用等待 `shutdown_complete_`。等待期间仍须允许另一个调用将优雅关闭升级为立即关闭。关闭正常返回须保证任务已处理、worker 已退出；立即关闭不能降级。

构造失败须回收已创建 worker，不能留下仍访问池的线程。析构采用优雅关闭，已立即关闭时保持该方式；析构前调用方协调所有外部访问。本池 worker 内析构本池调用 terminate。

本池 worker 的 wait 与两个关闭接口须在等待或改变状态前抛出调用错误；重复关闭规则不能绕过检查。不增加另一套关闭请求接口。

worker 集合在池成员中最后声明，逆序析构时，其依赖的同步对象仍然存活。

## 8. 待确认事项

已确定的默认 worker 数量、FIFO 边界、异常类型、结果类型范围和 wait 规则均已归入本文或 API 契约，不再列为未决事项。

| 主题 | 待确认内容 |
| --- | --- |
| 公共状态查询 | 是否增加公共状态查询接口；当前只定义状态枚举 |
| submit 诊断 | 是否增加显式模板约束和更完整的编译期诊断；当前依赖返回类型推导及右值引用结果的 static_assert |

后续实现与验证任务见 [IMPLEMENTATION.md](IMPLEMENTATION.md)，其中“待实现”不表示设计规则尚未确定。
