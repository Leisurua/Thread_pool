# API 契约

[项目入口](../README.md) · [内部设计](DESIGN.md)

使用方须包含 `thread_pool.hpp` 并链接 `thread_pool::thread_pool`。线程池不可复制、不可移动；worker 数量固定，队列无界。等待任务结果使用各自的 future，没有全池 wait、暂停恢复或单任务取消接口。

```cpp
explicit thread_pool(std::size_t worker_count = default_worker_count());
~thread_pool();

template<class F, class... Args>
auto submit(F&& function, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>&,
                                       std::decay_t<Args>&&...>>;

void shutdown();
void shutdown_now();
```

## 构造与析构

省略 worker 数量时采用 `std::thread::hardware_concurrency()`，返回 0 时保底 1；显式传入 0 抛 `std::invalid_argument`。构造分配或线程创建失败时，先回收已创建线程，再传播原异常。

析构默认优雅关闭，排空已接受任务并等待全部 worker 退出；显式关闭已完成时不重复回收。析构前，调用方须结束其他线程对池的访问；并发关闭支持不表示允许与池对象销毁并发访问。

本池 worker 内析构本池、关闭负责人在外层关闭未完成时重入析构本池，均导致 `std::terminate`。显式关闭的自等待规则见下文。

## submit：保存、接受与异常

callable 和参数按值保存：左值复制、右值移动。执行时，保存的 callable 作为左值调用，保存的参数作为右值传入；支持 move-only 对象、值/void/左值引用结果，不支持仅右值可调用对象或右值引用结果。

借用对象使用 `std::ref` / `std::cref`，由调用方保证对象、引用结果的生命周期和并发访问安全。按值保存不深拷贝指针所指对象，也不改变引用捕获；不可复制的左值须显式移动或借用。

成功入队是接受边界，返回 future，不等待执行结束；FIFO 约束领取顺序，不保证多 worker 的调用开始或完成顺序。任务可能在 submit 返回前执行完，也可能在返回前被立即关闭丢弃，此时仍返回对应 future。

| 情况 | 反馈 |
| --- | --- |
| 准备 callable/参数/共享状态，或加锁、入队失败 | submit 同步传播原异常，不返回 future，任务未被接受 |
| 到达入队检查时池已关闭 | submit 同步抛线程池产生的 `std::runtime_error`，不接受任务 |
| 已入队的任务执行或结果存储异常 | 异常保存到共享状态，由 future.get() 按原类型抛出 |

准备发生在状态检查之前，即使池已关闭，用户对象复制/移动等准备异常仍先传播；原异常即使也是 runtime_error，也不重新包装。调用前的参数表达式求值按普通 C++ 规则处理。已经发生的输入移动不回滚；输入被移动不等于任务已接受。入队分配失败时已有队列任务保持不变，用户捕获在队列锁外清理。

## future：结果与生命周期

`wait()` 只等待就绪，不获取结果或抛所存任务异常；`get()` 取值或抛所存异常，并消耗结果，不能再次对同一 future 调用 get。不读取 future 时，不主动向调用方抛任务异常；关闭也不汇总它们。

未领取任务被立即关闭丢弃后，future 就绪，`get()` 抛 `std::future_error`，错误码为 `std::future_errc::broken_promise`，可与 `std::make_error_code(...)` 比较。原 callable 不执行，关闭接口不以同步异常报告任务丢弃。

future 就绪不表示 worker 已退出或捕获对象已销毁。关闭返回后池和 worker 已释放任务所有权，调用方持有的 future 仍可能延长共享状态及捕获生命周期；不保证捕获在某个固定线程析构。

## shutdown / shutdown_now

两种接口均阻塞、返回 void，关闭生效后拒绝新任务，关闭后不重启。

| 首次生效的接口 | 已领取任务 | 未领取任务 | 正常返回条件 |
| --- | --- | --- | --- |
| shutdown() | 等待执行结束 | 继续领取执行 | 全部 worker 已 join |
| shutdown_now() | 等待执行结束 | 丢弃，future 反馈 broken_promise | 丢弃反馈完成，全部 worker 已 join |

已领取指 worker 取得队首任务并出队，即使尚未进入 callable。入队、领取和关闭的先后按同步边界内的实际生效顺序判断，不以函数调用开始时刻判断。异常结束也算执行结束；立即关闭不强制中断已领取任务，任务不结束时两种关闭都可能持续阻塞。

池外线程可以并发或重复关闭：首次生效的方式保持到底，后续调用等待同一次关闭完成，不升级为立即关闭，也不恢复已丢弃任务；关闭已完成时直接返回，不重复 join。

立即关闭准备阶段若抛异常，原异常同步传播，本次调用不改变池状态、队列或负责人身份，可重试；并发情况下其他调用仍可能生效。该保证限于准备阶段，不是任意关闭失败的全程回滚。具体标准库分配点与本机注入验证见 [设计说明](DESIGN.md#关闭流程)。

## 自等待与调用方责任

本池 worker 调用本池任一显式关闭接口，或同线程关闭负责人重入关闭本池，均在状态修改和等待前抛 `std::logic_error`。其他池的 worker 不因来源身份而被直接拒绝；重复调用不能绕过自等待检测。

任务内未捕获的 logic_error 进入该任务 future，worker 可继续处理其他任务。捕获对象析构中重入关闭或 submit 时，须自行处理 logic_error 或提交拒绝异常，避免异常逃逸 noexcept 析构。

调用方须避免跨池/跨线程循环等待，以及全部 worker 阻塞等待本池尚未执行任务的 future。检测只处理本池 worker 自关闭和同线程负责人重入，不解决一般等待环。

当前验证范围及未触发路径见 [README](../README.md#测试与验证)。
