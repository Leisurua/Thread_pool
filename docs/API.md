# C++20 线程池 API 契约

[项目入口](../README.md) · [内部设计](DESIGN.md) · [实现进度与验证](IMPLEMENTATION.md)

本文描述已确认精简方向及补充决定对应的目标契约。本轮仅同步文档，目标契约尚未迁移到源码；文档确认后再逐模块实现。当前源码仍有暂停、快照 wait、任务编号、取消标志和自定义异常，不能按本文假定这些行为已经改变。

当前已实现 Task、submit、work_thread 生命周期与 run、finish_task、worker 身份判定、默认 worker 数量及五种旧异常类型。池构造、析构、公共关闭仍只有声明，完整线程池尚不能直接使用。实际进度与现有测试边界见 [IMPLEMENTATION.md](IMPLEMENTATION.md)。

## 1. 范围与目标接口

固定 worker 数量、无界 FIFO 队列，保留泛型提交、future 结果反馈、优雅关闭和立即关闭。线程池与 work_thread 均不可复制、不可移动。不增加公共接口。

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

目标删除 pause()/resume()、池的 wait()/wait_result、任务 ID、历史取消记录、自定义异常体系、取消标志及关闭升级。future.wait()/get() 保留，用于各自任务的等待与取值；不增加替代性的全池等待或取消接口。

暂不实现动态 worker、有界队列、任务窃取和优先级调度。

## 2. 并发边界

| 概念 | 判定 |
| --- | --- |
| 接受任务 | submit 在主锁内成功入队 |
| 已领取任务 | worker 在主锁内取得队首 Task 并出队，即使尚未进入 callable |
| 尚未领取任务 | 仍在队列内，没有被 worker 取得 |
| 执行结束 | 正常返回或异常结束，不要求成功 |
| 关闭生效 | 首次关闭者在主锁内将 RUNNING 改为一种关闭状态 |
| 关闭完成 | 丢弃反馈（如果需要）已完成，全部 worker 已 join，并发布 TERMINATED |

FIFO 保证主锁内领取顺序，不保证多个 worker 实际调用开始顺序或完成顺序。入队、领取和关闭按同一主锁内的生效顺序判定先后，不以函数开始调用的时刻判定。

用户任务执行、捕获对象销毁及 worker join 均在主锁外；条件变量等待释放主锁。捕获对象的析构可能由 future 共享状态延后，不能保证发生在某个固定线程。

## 3. 构造与析构（待实现）

省略数量时采用 std::thread::hardware_concurrency()，返回 0 时保底采用 1；显式传入 0 时抛 std::invalid_argument。默认数量函数已实现，池构造仍待实现。构造成功后为 RUNNING；部分 worker 创建失败时，先通知并回收已创建线程，再传播原异常。

析构采用优雅关闭，等待排队任务、已领取任务和全部 worker 结束；此前显式关闭完成后不重复 join。关闭方式一经生效不改变，也不恢复已丢弃任务。

析构前调用方必须协调全部外部访问，使其他线程不再使用该对象。并发关闭支持不意味着可以与池对象的销毁并发访问。

本池 worker 内析构本池调用 std::terminate。关闭负责人在外层关闭尚未完成时重入析构本池也调用 std::terminate，不能销毁仍被外层关闭使用的对象。显式关闭接口的可捕获错误规则见第 6 节。

## 4. submit 与 future（现有实现待精简）

### 4.1 保存与调用语义

callable 和参数默认按值保存：左值复制、右值移动。执行时，保存的 callable 作为左值调用，保存的参数作为右值传入；实际调用与返回类型推导一致。

沿用现有支持范围：move-only callable/参数、仅左值可调用对象、值/void/左值引用结果；不支持右值引用结果或仅右值可调用的对象。保留已有 static_assert，不另增模板约束体系。

借用对象须显式使用 std::ref/std::cref，由调用方保证生命周期并协调并发访问；引用包装不移动目标对象。需要非 const 左值引用参数时使用 std::ref。按值保存不深拷贝指针所指对象，也不改变引用捕获；不可复制的左值须显式移动或借用。引用结果的生命周期同样由调用方保证。

### 4.2 接受、拒绝与失败

仅 RUNNING 接受新任务；SHUTDOWN、SHUTDOWN_NOW 和 TERMINATED 均拒绝提交。

| 情况 | 目标结果 |
| --- | --- |
| 成功入队先于关闭生效 | 返回 future，按首次生效的关闭方式执行或丢弃 |
| 关闭生效先于入队 | 同步抛 std::runtime_error，不入队、不返回 future |
| 准备、加锁或入队失败 | 同步传播原异常，不包装、不返回 future，任务未被接受 |

锁外准备 callable、参数及 Task；锁内检查状态并入队。入队失败时队列中已有任务保持原状，不再登记或回滚 ID/set。已发生的输入移动不回滚，关闭拒绝也不保证输入保持原状；输入被移动不等于任务已接受。

用户对象在准备阶段抛出的异常直接传播；即使类型也是 runtime_error，也不转换为另一种“提交失败”异常。调用 submit 前的参数表达式求值异常按普通 C++ 调用规则传播。

任务已成功入队但 submit 尚未返回时被立即关闭丢弃，仍返回对应 future，由该 future 反馈 broken_promise，不改为同步拒绝。

### 4.3 返回与任务异常

成功入队后通知 worker 并返回 std::future<R>，不等待任务执行结束。submit 可能等待主互斥锁，任务也可能在 submit 返回前完成。

Task 保留现有非模板外层、base/model 和 packaged_task 结构，删除取消入口与取消标志，不引入新的任务包装机制。任务执行抛出的异常由 packaged_task 保存，future.get() 按原类型抛出，保留原异常信息；不再包装成 task_execution_error，也不需要 original_exception() 查询。

future.wait() 只等待就绪，不获取结果或抛所存任务异常。get() 获取结果或抛所存异常后，结果已消耗，不能再次调用同一个 future 的 get()。不读取 future 时，不主动向调用方抛任务异常。

future 就绪不表示 worker 已退出或捕获对象已经销毁。关闭返回时池和 worker 已释放对任务的所有权，但调用方持有的 future 仍可能延长共享状态及捕获对象生命周期。

以下是目标生命周期完成后的示例，当前不能作为完整线程池示例独立链接运行：

```cpp
thread_pool pool(2);
auto result = pool.submit([](int a, int b) { return a + b; }, 3, 4);
int value = result.get();
pool.shutdown();
```

## 5. shutdown / shutdown_now（待实现）

### 5.1 任务处理与返回

| 首次生效的接口 | 已领取任务 | 尚未领取任务 | 正常返回条件 |
| --- | --- | --- | --- |
| shutdown() | 等待执行结束 | 继续 FIFO 领取并执行 | 所有 worker 已 join |
| shutdown_now() | 等待执行结束 | 移出队列，在主锁外销毁 Task | 丢弃反馈已完成，所有 worker 已 join |

两种接口均阻塞、返回 void，开始关闭后拒绝新提交。异常结束也算执行结束；正常返回不要求先调用 future.get()，关闭不汇总任务异常。

shutdown_now() 不强制中断已领取任务；“立即”不表示立即返回。已领取任务即使尚未进入 callable，也继续执行。任务不结束时，关闭可能持续阻塞。

立即关闭丢弃任务时不执行原函数、不调用 packaged_task 来制造取消异常。销毁尚未执行的 packaged_task，使结果提供方放弃共享状态；对应 future 就绪，get() 抛 std::future_error，错误码为 std::future_errc::broken_promise。调用方可比较 error.code() 与 std::make_error_code(std::future_errc::broken_promise)。

丢弃反馈通过 future 提供，不使 shutdown_now() 同步抛“任务取消”异常。

### 5.2 并发与重复调用

池外线程允许重复或并发关闭。首次在主锁内生效的调用确定关闭方式并成为唯一关闭负责人，负责清理和 join；其他调用者等待同一次关闭完成，不重复访问或 join worker 集合。

| 当前情况 | 后续 shutdown() | 后续 shutdown_now() |
| --- | --- | --- |
| 优雅关闭中 | 等待优雅关闭完成 | 等待优雅关闭完成，不丢弃队列 |
| 立即关闭中 | 等待立即关闭完成，不恢复任务 | 等待立即关闭完成 |
| 已关闭，worker 已 join | 直接返回 | 直接返回 |

两种关闭方式之间不升级、不降级。首次按锁内生效顺序判断，不按调用开始时刻判断；并发使用不同关闭接口不因方式不同报错。

第 6 节的自等待检测优先于上述等待/返回规则，不能借重复调用绕过检测。

## 6. 自等待检测与调用方边界（待接入）

本池 worker 调用本池 shutdown() 或 shutdown_now()，在状态修改和等待前同步抛 std::logic_error。已有 worker 身份登记与判定继续保留，仅匹配本池，不将其他池 worker 误判为本池 worker。

正在关闭本池的负责人线程再次调用本池任一关闭接口，在进入等待前同步抛 std::logic_error；不改变外层关闭状态、方式或进度，也不重复 join。内部仅增加一个由主锁保护的负责人线程 ID，不扩展公共 API。

负责人身份覆盖首次关闭生效到清理丢弃任务、join 和发布完成的整个期间。捕获对象析构在该线程清理任务时重入关闭可被识别；它必须自行捕获 logic_error，不能让异常逃逸 noexcept 析构。捕获对象若在关闭期间 submit 到本池，也须处理提交拒绝异常。

未捕获的 worker 自关闭 logic_error 按原类型进入该任务 future，worker 仍可继续处理其他任务。析构自等待采用 terminate，见第 3 节。

检测不解决跨线程或跨池的循环等待。任务内 submit 仍可使用，但若全部 worker 阻塞等待本池排队任务的 future，可能使池无法推进；调用方须避免这些等待环，不引入协作等待或任务窃取。

## 7. 目标异常反馈汇总

| 情况 | 反馈位置与类型 |
| --- | --- |
| 关闭后拒绝提交 | submit 同步抛 std::runtime_error |
| 保存、分配、加锁、入队失败 | submit 同步传播原异常 |
| 正常结果 | future.get() 获取值、void 或左值引用 |
| 任务函数异常、结果存储异常 | future.get() 按原异常类型反馈 |
| 未领取任务被立即关闭丢弃 | future.get() 抛 std::future_error(broken_promise) |
| worker 自关闭、关闭负责人重入显式关闭 | 对应关闭接口同步抛 std::logic_error |
| worker_count 为 0 | 构造同步抛 std::invalid_argument |
| worker 或关闭负责人重入析构本池 | std::terminate |

以上均为精简目标；旧代码的异常类型和实际测试覆盖保留在 IMPLEMENTATION.md 的当前进度部分，待对应模块实现时更新。
