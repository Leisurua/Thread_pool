# 线程池 API 契约

[项目入口](../README.md) · [内部设计](DESIGN.md) · [实现进度](IMPLEMENTATION.md)

接口来源为 [thread_pool.hpp](../include/thread_pool.hpp)，worker 循环位于 [thread_pool.cpp](../src/thread_pool.cpp)。当前已实现 `Task`、`submit()`、work_thread 生命周期与 run、finish_task、worker 身份判定及四种提交/任务异常。池构造与析构、pause/resume、关闭和 wait 仍待实现，完整线程池尚不能直接使用。

本文“已实现”指对应产品代码已有定义；“待实现”章节描述既定契约。worker 测试通过池生命周期和状态桩驱动真实产品线程，不代表公共 pause/resume、关闭或 wait 接口已完成。

## 1. 通用边界

| 概念 | 判定 |
| --- | --- |
| 接受任务 | submit 成功入队，不以调用开始或返回时刻判定 |
| 正在执行 | worker 为执行任务成功领取并出队，即使尚未进入 callable |
| 尚未执行 | 尚未被 worker 成功领取 |
| 执行结束 | 正常返回或异常结束，不要求成功 |
| 处理结束 | 已执行结束，或已被丢弃并完成 future 反馈 |

V1.0 固定 worker 数量，运行期间不动态增减。FIFO 保证领取顺序，不保证多个 worker 的实际调用开始顺序或完成顺序。

状态、队列、wait 快照和进度、单一有效 wait 资格由统一主互斥锁协调，相关操作以锁内生效顺序判定先后。任务执行与 worker join 在锁外；条件变量等待释放主锁。状态枚举不等于公共状态查询接口。

## 2. 构造与析构（待实现）

当前声明：

```cpp
explicit thread_pool(std::size_t worker_count = default_worker_count());
~thread_pool();
```

省略数量时采用 `std::thread::hardware_concurrency()`，返回 0 时采用 1；显式传入 0 抛出 `std::invalid_argument`。构造成功后为 `RUNNING`；部分 worker 创建失败时，须回收已创建线程后传播失败。

线程池与 `work_thread` 均不可复制、不可移动。

析构采用优雅关闭语义，等待任务处理结束和所有 worker 退出；暂停不阻止关闭。若已进入立即关闭，则保持该方式，不恢复已丢弃任务；已退出则不重复关闭。任务结果继续通过 future 反馈。

析构前调用方必须协调所有外部访问。本池 worker 内析构本池调用 `std::terminate()`，不采用普通阻塞接口的抛异常规则。

## 3. submit：提交任务（已实现）

### 3.1 签名与保存语义

```cpp
template<class F, class... Args>
auto submit(F&& function, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>&,
                                       std::decay_t<Args>&&...>>;
```

callable 和参数默认按值保存：左值复制，右值移动。执行时，保存的 callable 作为左值调用，保存的参数作为右值传入；实际调用与返回类型推导一致。支持 move-only 对象、仅左值可调用的函数对象，以及值、`void` 和左值引用结果；不支持右值引用结果或仅右值可调用的函数对象。

借用对象须显式使用 `std::ref` / `std::cref`，由调用方保证生命周期并协调并发访问；引用包装不移动目标对象。需要非 const 左值引用参数时可使用 `std::ref`。按值保存不深拷贝指针所指对象，也不改变引用捕获；不可复制的左值须显式移动或借用。引用结果的生命周期同样由调用方保证。

### 3.2 接受、拒绝与失败

`RUNNING` 和 `PAUSED` 接受新任务；`SHUTDOWN`、`SHUTDOWN_NOW` 和 `TERMINATED` 拒绝新任务。

| 情况 | 结果 |
| --- | --- |
| 成功入队先于关闭生效 | 返回 future，按最终关闭方式执行或丢弃 |
| 关闭生效先于入队 | 同步抛出 `submission_rejected`，不入队、不返回 future |
| 准备、加锁、编号检查或入队失败 | 同步抛出 `submission_error`，不返回 future，任务未被接受 |

入队失败时，队列和已接受任务边界保持原状，本次未完成记录回滚。已发生的输入移动不回滚；因关闭拒绝时也不保证输入保持原状。输入被移动不等于任务已接受。

用户对象构造时抛出的 `submission_rejected` 仍属于提交失败，包装为 `submission_error`。调用 submit 前的参数表达式求值异常不属于包装范围。

任务已入队、但 submit 尚未返回时被立即关闭丢弃，仍通过返回的 future 反馈 `task_cancelled`，不改为提交拒绝。

### 3.3 返回与等待

成功入队后通知 worker 并返回 `std::future<R>`，不等待任务执行结束。submit 可能等待主互斥锁，任务也可能在 submit 返回前完成。

future 就绪与池内处理进度登记是不同事件。worker 在任务调用结束后释放自身持有的 Task，再调用 finish_task；future.wait()/get() 返回时，进度登记可能尚未完成。future 仍可能持有结果共享状态，借用对象、引用结果及捕获对象的生命周期不能仅依据池内进度推断。

以下示例说明完成生命周期实现后的调用方式，当前不能作为完整线程池示例独立运行：

```cpp
thread_pool pool(2);
auto result = pool.submit([](int a, int b) { return a + b; }, 3, 4);
// 提交后可继续执行其他操作。
int value = result.get(); // 等待就绪并获取结果，或抛出所存异常。
```

future 只提供结果等待与读取能力，不直接提供任务编号、取消或强制中断接口。内部准备与入队流程见 [提交设计](DESIGN.md)。

## 4. future 与异常反馈

| 类型或情况 | 反馈位置 | 基类 |
| --- | --- | --- |
| `submission_rejected`：关闭后拒绝提交 | submit 同步抛出 | `std::exception` |
| `submission_error`：成功入队前失败 | submit 同步抛出 | `std::exception` |
| 正常返回 | future.get() 获取结果 | — |
| `task_cancelled`：已接受任务被丢弃 | future.get() 抛出 | `std::exception` |
| `task_execution_error`：原任务函数调用失败 | future.get() 抛出 | `std::exception` |
| packaged_task 内部存储结果失败 | future.get() 按原类型抛出 | 原异常类型 |
| `thread_pool_call_error`：非法状态操作、并发 wait 或本池 worker 自等待 | 对应接口同步抛出 | `std::logic_error` |

`submission_rejected`、`submission_error`、`task_cancelled` 和 `task_execution_error` 已实现；`thread_pool_call_error` 的构造函数仍只有声明，公共接口的调用错误检查也尚未接入。

`submission_error` 与 `task_execution_error` 均提供：

```cpp
std::exception_ptr original_exception() const noexcept;
```

查询只返回原异常句柄，不强制重抛。调用方可持有或跨线程传递句柄，按需调用 `std::rethrow_exception`；句柄维持原异常生命周期，不依赖包装对象继续存活。该选择尚无整体性能保证。

`future.wait()` 只等待就绪，不取出或抛出任务异常；不调用 get() 时，不主动向调用方抛出所存异常。get() 返回结果或抛出所存异常后，结果已被消耗，不能再次对同一个 future 调用 get()。

丢弃不使 shutdown_now 同步抛出丢弃异常。任务异常不应使 worker 退出或线程池停止；关闭和 wait 不汇总任务异常，也不要求调用方先读取 future。

## 5. pause / resume：暂停与恢复（待实现）

当前声明：`void pause()`、`void resume()`。

pause 暂停新的任务领取，保留队列；已领取任务继续执行。它不等待这些任务结束，因此返回后仍可能有任务执行，甚至此前已领取的 callable 才实际开始执行。

暂停期间允许 submit，新任务排队但暂不执行。resume 恢复领取任务。未开始关闭时，重复 pause 或重复 resume 均保持状态并直接返回。

开始关闭后及关闭完成后，两种接口均同步抛出 `thread_pool_call_error`，不改变状态、关闭方式或关闭进度。与关闭并发时，按主锁内生效顺序判定先后。

## 6. shutdown / shutdown_now：阻塞关闭（待实现）

当前声明：`void shutdown()`、`void shutdown_now()`。

### 6.1 任务处理与返回

| 接口 | 已领取任务 | 尚未领取任务 | 正常返回条件 |
| --- | --- | --- | --- |
| shutdown()，未被升级 | 等待执行结束 | 继续执行并等待结束 | 全部任务执行结束，worker 退出 |
| shutdown_now() | 等待执行结束 | 丢弃，通过 future 反馈 | 已领取任务结束，其余任务已丢弃，worker 退出 |

两种接口开始关闭后均拒绝新提交，保持 void 返回；异常结束也满足执行结束条件。正常返回不要求先调用 future.get()。

shutdown 优先于暂停，自动允许排队任务执行，无需额外 resume。暂停期间 shutdown_now 直接丢弃排队任务，等待已领取任务结束。

shutdown_now 不强制中断已领取任务，“立即”不表示立即返回；任务不结束时，关闭可能持续阻塞。

### 6.2 重复调用与升级

池外线程允许重复或并发关闭，共用一次最终关闭与 worker join，不因重复或关闭方式不同报错。

| 当前情况 | 后续 shutdown() | 后续 shutdown_now() |
| --- | --- | --- |
| 优雅关闭中 | 等待最终关闭完成 | 升级为立即关闭，等待完成 |
| 立即关闭中 | 保持立即关闭，等待完成 | 保持立即关闭，等待完成 |
| 已关闭，worker 已退出 | 直接返回 | 直接返回 |

升级丢弃尚未领取任务，已领取任务继续执行。原 shutdown 调用仍等待最终退出并以 void 返回，但不再保证全部已接受任务执行。立即关闭不能降级，也不恢复被丢弃任务。

## 7. wait：等待任务快照（待实现）

当前声明：`thread_pool::wait_result wait()`。

wait 在主锁内确定本次接受边界，等待边界前全部已成功入队任务处理结束，包括排队和已领取任务；已处理结束的任务无需继续等待，尚未成功入队的提交不属于快照。

边界后的任务不扩展范围，其完成不能代替快照任务完成。wait 返回时整个池不必空闲，后续任务可能仍排队或执行。

| 返回值 | 条件 |
| --- | --- |
| `completed` | 快照内全部执行结束，没有丢弃；异常结束也算结束 |
| `cancelled` | 快照内至少一个任务被丢弃，其余任务也全部执行结束或被丢弃 |

发现丢弃不提前返回；两种结果都保证快照全部处理结束。边界前全部已接受任务中的历史丢弃仍计入 cancelled，快照外丢弃不影响结果。

同一时刻只允许一个有效 wait；其他并发调用同步抛出 `thread_pool_call_error`，不影响当前等待、状态或任务。前一次结束后，后续调用确定自己的新快照。

暂停、关闭中和关闭后均允许池外线程调用 wait。暂停时可能需要另一线程恢复或关闭；固定快照不保证任务在有限时间内结束。

wait 不关闭线程池，不保证 worker 已退出，也不使已关闭的池重新运行。未被其他线程关闭时，返回后线程池仍可使用。

## 8. 本池 worker 的调用限制（待实现）

本池 worker 禁止调用本池 shutdown、shutdown_now 或 wait，均同步抛出 `thread_pool_call_error`。

内部 worker 身份登记与判定已实现，只将当前线程所属池匹配为本池；三个公共阻塞接口中的拒绝检查尚待实现。

关闭接口必须在状态变化或关闭开始前拒绝非法调用；wait 不进入等待、不改变状态，也不排除当前任务构造特殊快照。重复关闭规则不能绕过 worker 检查。

若任务未捕获该错误，则包装为 `task_execution_error`，经该任务 future 反馈，不影响其他任务。worker 内析构本池的规则见第 2 节。
