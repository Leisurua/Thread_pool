# C++20 线程池精简设计

[项目入口](../README.md) · [目标 API 契约](API.md) · [当前实现进度](IMPLEMENTATION.md)

精简方向已审核通过，本文纳入用户补充决定，并与 API.md、IMPLEMENTATION.md 同步。本轮仅修订这三份文档，不修改源码或测试；文档确认后再逐模块实现。本文描述目标设计，当前源码仍是旧版本，不能将目标行为视为已经实现。

已确定：丢弃任务使用 broken_promise；支持并发关闭，首次关闭者负责 join，其他调用者等待，不升级关闭方式；保留 worker 自关闭检测，增加最小化的关闭负责人重入检测；保留现有 Task 类型擦除；用户任务执行、捕获对象销毁和 worker join 均在主锁外；不增加功能或公共 API。

当前源码已完成 Task、submit、work_thread 生命周期、worker 循环、线程身份判定、默认 worker 数量及快照进度登记。池构造、析构和公共关闭接口仍只有声明，完整线程池尚不能直接使用。现有两项测试使用池生命周期桩，不能证明公共生命周期正确。

## 1. 目标范围

固定 worker 数量、无界 FIFO 队列，使用 C++20、std::jthread、std::packaged_task 和 std::future。保留泛型提交、Task 类型擦除、原异常反馈、优雅关闭、立即关闭以及维持这些行为所需的同步机制。

删除 pause/resume、快照 wait、任务 ID、历史取消记录、自定义异常包装、取消标志、关闭升级及其专用成员。暂不实现动态 worker、有界队列、任务窃取、优先级调度；也不新增状态查询、取消单个任务、全池 wait 或统计 API。

任务等待使用各自的 future；最终回收使用阻塞关闭。项目完成标准是能够直接链接使用、契约清楚、有行为测试和可复现的性能数据。

## 2. 源码取舍与依赖关系

### 2.1 保留的实现

| 实现 | 保留原因与边界 |
| --- | --- |
| submit 的返回类型推导、按值捕获、tuple/apply/invoke | 已支持 move-only callable/参数、std::ref、值/void/左值引用结果；保留现有调用语义 |
| std::packaged_task 与 std::future | 沿用标准库共享状态，不换成自建 promise 结果通道 |
| Task 的非模板外层、unique_ptr<base>、model<R> | 保留既有类型擦除，禁用复制，支持 noexcept 移动 |
| std::queue<Task> | 保留 FIFO；只保证主锁内领取顺序，不保证多 worker 的调用开始或完成顺序 |
| work_thread、jthread、worker 集合 | 保留现有封装与固定数量，不改为另一套线程架构 |
| mutex_、task_ready_ | 统一保护状态与队列，使用带谓词的条件变量等待 |
| worker_identity_guard、current_worker_pool_、is_current_worker | 防止本池 worker 阻塞关闭自身池；保留现有 TLS 与 RAII 实现 |
| shutdown_complete_ | 保留用于并发关闭调用等待 TERMINATED，不删除并发关闭支持 |
| default_worker_count、禁用池/worker 复制与移动 | 硬件并发数返回 0 时保底 1；维持地址与生命周期稳定 |
| 锁外准备、执行及销毁，静态库结构 | 保留已完成的同步边界与 include/src 分工 |

### 2.2 删除或修改的实现

| 删除或修改项 | 直接依赖与连带改动 |
| --- | --- |
| pause()、resume()、PAUSED、change_state() | 删除暂停接口和专用辅助声明；修改 submit 状态检查、worker 等待谓词、状态驱动测试 |
| wait()、wait_result、wait_active_ | 删除快照 API 与单一有效等待资格；不替换为另一种全池 wait |
| task_id_type、Task::id_ / id()、Task 的 ID 友元、last_accepted_id_ | 修改 Task 构造、submit 入队、worker 领取及测试；移除编号溢出逻辑 |
| unfinished_tasks_、first_cancelled_id_、finish_task()、progress_changed_ | 删除 set 登记/回滚、完成登记和历史取消判断；worker 完成后直接进入下一轮 |
| Task::cancel()、base/model 的 cancel、shared_ptr<bool> cancelled | 不再调用任务来制造取消异常；立即关闭通过锁外销毁未执行 Task 反馈 broken_promise |
| submission_error、task_execution_error、original_exception() | 删除捕获再包装；准备、入队失败和任务函数异常保持原类型 |
| submission_rejected、task_cancelled、thread_pool_call_error | 统一使用标准异常，见第 4 节；不保留自定义异常体系 |
| 关闭升级及 join_in_progress_ | 由 RUNNING 到关闭状态的唯一首次转换确定关闭负责人；去掉升级与额外 join 资格布尔值，见第 6 节 |
| 私有 closing_thread_id_（待增加） | 一个由 mutex_ 保护的 std::thread::id，仅用于关闭负责人重入检测；不是任务 ID 或额外的 join 资格标志 |
| 专用 include 与旧注释 | 使用点清理后移除 set、limits 及任务编号相关依赖；cstdint 是否保留取决于状态枚举底层类型 |

删除链条如下，各链条应连同对应测试一起修改，避免留下孤立成员：

```text
快照 wait
  → 接受边界 / Task ID / 未完成 set / 历史取消记录
  → submit 登记和回滚 / worker finish_task / progress_changed_

自定义取消反馈
  → task_cancelled / Task::cancel / model::cancel
  → 共享取消标志 / invocation 的取消分支

pause/resume
  → PAUSED / change_state
  → submit 接受状态 / worker 等待分支 / 暂停测试桩

关闭升级
  → 优雅关闭改为立即关闭 / 独立 join 资格协调
  → 改用首次关闭状态转换选出唯一负责人
```

shutdown 不依赖完成计数或未完成 set：优雅关闭时 worker 排空队列后退出，join 等待所有已领取任务和 worker 结束；立即关闭时先移出排队任务，join 等待已领取任务。没有快照 wait 后，无须新增 active_tasks_ 或原子完成计数。

## 3. 目标接口与状态

保留接口形状：

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

不增加新的公开接口。状态枚举保留 RUNNING、SHUTDOWN、SHUTDOWN_NOW、TERMINATED，删除 PAUSED；枚举不提供额外的公共查询行为。

| 状态 | 接受提交 | worker 行为 |
| --- | --- | --- |
| RUNNING | 是 | 队列非空时领取，否则等待 |
| SHUTDOWN | 否 | 排空队列；队列为空后退出 |
| SHUTDOWN_NOW | 否 | 不再领取；已领取任务执行结束后退出 |
| TERMINATED | 否 | 所有 worker 已 join，关闭完成 |

状态路径为 RUNNING → SHUTDOWN → TERMINATED，或 RUNNING → SHUTDOWN_NOW → TERMINATED。两种关闭状态之间不转换；关闭后不重启。

## 4. submit、Task 与异常反馈

### 4.1 保持现有泛型调用语义

callable 和参数按值保存：左值复制、右值移动；借用显式使用 std::ref/std::cref。保存的 callable 作为左值调用，保存的参数作为右值传入。沿用现有返回类型推导和右值引用结果 static_assert，不在此次精简中增加另一套模板约束。

支持值、void、左值引用结果；不支持右值引用结果和仅右值可调用对象。引用、指针目标和引用捕获的生命周期及并发访问仍由调用方保证。

锁外依次保存函数/参数、构造 packaged_task、取得 future、构造 Task；主锁内只检查 RUNNING 和将 Task 入队。成功入队是接受任务的边界，解锁后 notify_one 并返回 future。

不再分配任务编号或登记 set。失败时不返回 future，任务未被接受；已发生的输入移动不回滚。保持局部 Task 先于锁对象构造，使异常清理先解锁，再释放持有用户对象的 Task。

### 4.2 简化 Task，不改类型擦除架构

Task 外层仅持有 unique_ptr<base>；base 保留虚析构和 execute，model<R> 仅持有 packaged_task<R()>。移除编号、cancel 和取消标志。模型仍接管 submit 已准备的 packaged_task，不获取第二个 future、不捕获模型 this、不另建 promise。

invocation 直接 apply/invoke，不捕获原函数异常再包装。正常结果及原异常由 packaged_task 保存。每个被领取的 Task 只执行一次；队列和局部 Task 的移动所有权保证未领取任务销毁与已领取任务执行不会竞争同一任务。

现有 execute 的 noexcept 依赖有效且只调用一次的 packaged_task 不变量。用户函数或结果存储异常走 future；重复调用/无效状态等内部误用不能用 catch-all 静默吞掉，测试应防止这些路径。

### 4.3 标准异常契约

| 情况 | 反馈 |
| --- | --- |
| callable/参数保存、内存分配、加锁、入队失败 | submit 同步传播原异常，无包装 |
| 池已开始关闭，提交被拒绝 | submit 同步抛 std::runtime_error |
| 任务函数抛异常 | future.get() 抛原异常类型，保留原有异常信息 |
| packaged_task 内部存储结果失败 | future.get() 按标准库行为反馈原异常 |
| 已接受但未领取的任务被立即关闭丢弃 | 销毁未执行的 packaged_task，使 future 就绪；get() 抛 std::future_error，code() 为 future_errc::broken_promise |
| 显式 worker_count 为 0 | 构造抛 std::invalid_argument |
| 本池 worker 调用本池关闭接口 | 状态变化和等待前抛 std::logic_error |
| 正在关闭本池的负责人线程重入本池关闭接口 | 在再次关闭或进入等待前抛 std::logic_error，不影响外层关闭方式和进度 |

broken_promise 表示结果提供方放弃该共享状态，本项目将它用作未执行任务丢弃反馈；它不是新增的“取消”异常类型。丢弃时不调用原函数，也不调用 packaged_task 来模拟取消。该语义已核对本机 GCC 16.2.0 的 future 实现，须补充产品行为测试。

future.wait() 只等待就绪，get() 获取结果或抛所存异常；不读取 future 时不会主动向调用方抛任务异常。任务异常不使 worker 退出，关闭不汇总任务异常。

## 5. 同步规则与 worker 循环

mutex_ 保护 state_、tasks_ 及 closing_thread_id_。首次状态转换选取关闭负责人，线程 ID 仅检测负责人重入，不另设 join 资格。队列成功入队、成功领取和关闭生效都在此锁内线性化：提交先入队则被接受；关闭先生效则提交被拒绝。立即关闭先于领取则丢弃，领取先于立即关闭则继续执行，即使尚未进入 callable。

task_ready_ 谓词为“状态不是 RUNNING，或队列非空”。唤醒并持锁后，SHUTDOWN_NOW/TERMINATED 退出；SHUTDOWN 且队列为空退出；其余情况从队首移动 Task 并 pop。

解锁后执行局部 Task，再在锁外释放 worker 的 Task 所有权；删除 finish_task 调用。保留 worker 身份 RAII，不增加 stop_token 协议：jthread 用于线程生命周期回收，关闭仍由池状态和条件变量驱动。

用户函数、用户对象销毁、未执行 packaged_task 的销毁反馈以及 join 均在主锁外。等待 shutdown_complete_ 时释放主锁，谓词检查 TERMINATED。future 就绪不表示所有用户捕获已销毁；future 的共享状态仍可能延长对象生命周期。

## 6. 生命周期与关闭（待实现）

### 6.1 构造和析构

保持默认 worker 数量规则；显式 0 拒绝。先完成池状态、队列与同步成员初始化，再创建 worker。所有已创建 worker 的回收先于其依赖成员销毁。

创建部分 worker 后失败时，在主锁内设置立即退出状态，解锁并 notify_all，再在锁外回收已创建 worker，最后传播原构造异常。单靠 jthread 析构发出的 stop 请求不能唤醒现有 condition_variable，也不能改变池状态。

析构默认优雅关闭；显式关闭已经完成后不重复 join。析构前由调用方保证没有其他外部线程继续访问池。本池 worker 中析构本池调用 std::terminate，不能尝试 self-join；关闭负责人在外层关闭尚未完成时重入析构本池也调用 std::terminate，不能销毁仍被外层关闭使用的池。

保留 worker 集合最后声明，使逆序销毁时同步依赖仍存活。需要显式完成退出通知，成员顺序本身不能保证线程能退出。

### 6.2 两种关闭的任务边界

| 接口 | 已领取任务 | 尚未领取任务 | 正常返回条件 |
| --- | --- | --- | --- |
| shutdown() | 等待执行结束 | 继续 FIFO 领取并执行 | 全部 worker 已 join |
| shutdown_now() | 等待执行结束 | 移出队列并在锁外销毁，future 反馈 broken_promise | 丢弃反馈完成，全部 worker 已 join |

“立即关闭”不强制中断已领取任务，也不保证立即返回；任务不结束，关闭就可能持续等待。两种关闭开始后均拒绝提交。

立即关闭用局部队列承接待丢弃任务，主锁内交换队列所有权，主锁外销毁。所需局部容器在状态转换前准备，避免关闭已生效后因准备失败留下无人完成关闭的状态。发布 TERMINATED 前须完成丢弃反馈与 join。

### 6.3 并发与重复关闭

保留池外线程并发关闭：首次在主锁内将 RUNNING 改为任一关闭状态的调用是唯一关闭负责人，同时记录其线程 ID；解锁后通知 worker，在锁外完成丢弃（如果需要）和 join，最后持锁发布 TERMINATED、清空负责人 ID，解锁后通知 shutdown_complete_。后续调用在排除自等待后等待 TERMINATED，不访问或 join worker 集合。状态转换已经选出负责人，无须 join_in_progress_。

首次关闭方式保持到底：shutdown() 已生效后，shutdown_now() 只等待，不丢弃队列；反向调用也不恢复任务。已 TERMINATED 时重复调用直接返回。首次指锁内生效顺序，不是函数开始时间。

本池 worker 检查须早于主锁获取、状态修改和等待，即使另一个线程已经关闭该池，也不能绕过检查进入等待。

### 6.4 最小化的关闭负责人重入检测

增加一个私有成员 `std::thread::id closing_thread_id_{};`，所有读取与写入均持 mutex_，不使用原子变量，不新增 TLS 守卫、请求关闭 API 或任务包装。

关闭入口先用现有 TLS 拒绝本池 worker；随后持主锁，如果池处于任一关闭状态且 closing_thread_id_ 等于当前线程 ID，立即抛 std::logic_error，不改变状态、不再次 join、不进入 shutdown_complete_ 等待。否则由 RUNNING 的首次转换登记负责人，或按第 6.3 节等待/返回。初始化和关闭完成后，负责人 ID 均为空。

负责人身份覆盖从首次关闭生效到完成丢弃、join、发布 TERMINATED 的整个期间。局部 Task/待丢弃队列的清理必须在发布完成和清空身份之前结束，且在主锁外，以便捕获对象析构中的同步重入能被检测。关闭完成后的顺序重复调用仍正常返回。

如果用户析构中调用关闭，必须自行捕获重入检测的 logic_error，不能让异常逃逸 noexcept 析构。检测只针对同一线程正在关闭同一池：负责人关闭其他池不会被直接误判，但跨池/跨线程等待环仍由调用方避免。

## 7. 并发安全边界与确定方案

当前核心状态/队列操作共用主锁，取消标志在执行与取消互斥的所有权约定下不需要原子化。未发现必须通过重写 Task 或 worker 架构解决的数据竞争。以下是精简及后续生命周期实现必须处理的死锁边界；公共生命周期尚无实现，不能把测试桩当作已经解决这些问题。

| 问题与原因 | 确定方案与边界 |
| --- | --- |
| 构造失败回收：worker 正在 condition_variable 上睡眠，jthread 的 stop 请求不会使现有谓词成立 | 保持状态协议，退出状态 + notify_all + 锁外 join，不引入 stop_token 感知等待 |
| 本池 worker 阻塞关闭自身池：负责人会 join 自己；等待其他关闭负责人时，负责人又在等待该 worker | 复用 TLS 身份，关闭入口抛 logic_error，析构 terminate，不新增非阻塞请求接口 |
| 多个关闭调用直接操作 worker 集合：重复 join/容器访问可能竞争；持主锁 join 又会阻止 worker 领取/退出 | 首次状态转换选出负责人 + shutdown_complete_；只由负责人操作 worker 集合，其他调用等待 |
| 锁内销毁捕获对象：用户析构若重新 submit 会再次请求主锁而死锁 | 保留现有锁外销毁；立即关闭用队列交换后锁外清理，拒绝提交和入队失败也须保持解锁后清理 |
| 关闭负责人清理丢弃任务时，捕获对象析构再次阻塞关闭本池：即使在主锁外，也会等待自身发布 TERMINATED | 用主锁保护的 closing_thread_id_ 检测同线程重入，进入等待前抛 logic_error；用户析构自行捕获，外层关闭继续 |
| 外部关闭线程在等 worker，而 worker 在等该线程、或在等队列中无法执行的任务 | 明确要求任务之间及调用方不能形成循环等待；同池任务中 submit 可用，但阻塞 get 可能耗尽全部 worker，此次不引入协作等待或任务窃取 |

捕获对象的析构应不抛异常；它若尝试 submit 到正在关闭的池，须自行处理拒绝异常，避免异常逃逸析构导致 terminate。锁外销毁解决主锁重入，不能消除所有用户层等待环。

broken_promise、首次关闭方式保持到底、并发等待、worker 自关闭检测与关闭负责人重入检测均已由用户确定。检测不宣称消除任意用户层等待环；实现时若发现新的并发问题，先说明原因及可选方案，再讨论决定。

## 8. 分模块推进与验证

本轮已同步 API.md 和 IMPLEMENTATION.md，明确目标与实际进度；等待本轮文档确认，仍不修改源码或测试。确认后的源码阶段每轮只处理下列一个功能模块，同时调整其直接依赖的测试，验证后停止，不自动继续下一模块。

| 次序 | 单一功能模块 | 修改与验证范围 |
| --- | --- | --- |
| 1 | 任务结果与异常反馈 | 保留 Task/model/packaged_task，移除异常包装和取消标志/cancel，采用第 4 节标准异常；更新原异常、准备失败、结果存储失败、销毁任务反馈测试，运行 submit 和 worker 测试 |
| 2 | 移除快照进度 | 删除 wait、ID、set、历史取消、finish_task 与专用成员；同步去掉 submit/worker 登记；保留多生产者恰好执行一次、锁外准备和 FIFO 的测试，重新设计不依赖 set 的队列分配失败注入 |
| 3 | 移除暂停恢复 | 删除接口、PAUSED 和辅助声明，简化状态谓词；用 latch/受控阻塞任务组织 FIFO 和未领取任务测试，去掉 PAUSED 初始化桩，运行 worker 与 submit 测试 |
| 4 | 池生命周期与两种关闭 | 完成构造/失败回收/析构及共用关闭流程；移除测试生命周期桩，验证公共接口可直接链接与使用，并覆盖并发关闭、无升级、worker 自关闭拒绝、负责人重入拒绝、排空、丢弃和析构等待 |
| 5 | 性能验证 | 正确性模块通过后单独建立最小基准和结果记录；不新增线程池功能，性能数据跑出后再用于 README/简历 |

模块 1 至 3 的测试可以继续使用未实现生命周期的桩，但要保留显著说明。产品生命周期开始提供定义时，应同步删除两个测试各自的构造/析构定义，避免重复符号和继续绕过产品接口。

保留现有 move-only、std::ref、左值调用、引用/void 结果、锁外嵌套 submit、异常后 worker 继续运行的测试价值；删除只验证 ID 溢出、历史取消、暂停和快照登记的断言。多 worker 乱序完成可验证后面的 future 先就绪，不再借助 ID/set 判断进度。

关键同步测试优先使用 latch、promise/future 和受控任务建立先后关系；超时用于防止挂死，不把 sleep 当作关闭已生效的证明。新增生命周期测试须验证：被丢弃 callable 未被调用、future 全部就绪、已领取任务继续完成、无漏执行/重复执行、重复关闭安全。负责人重入测试应控制捕获对象的最后一个共享状态引用，确保其在关闭线程清理队列时析构；覆盖两种显式关闭接口重入均抛 logic_error，且捕获后外层关闭仍能完成，避免 future 延迟析构使测试绕过目标路径。构造部分失败需要可控测试注入，避免依赖实际耗尽系统线程资源；是否加入测试专用接缝在该模块内决定。

每轮按本机约定执行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --test-dir build/ucrt64-debug --output-on-failure
```

### 性能数据范围

使用现有 ucrt64-release 预设，记录 CPU、OS、编译器、构建模式、worker 数量、生产者数量、任务数、任务负载和计时边界。比较相同计算工作的直接串行执行、单 worker 和多 worker 执行；任务计算应有可观测校验值，避免优化掉工作。

分别报告批量 submit 的平均提交耗时，以及从首个提交到全部 future.get 完成的总耗时/吞吐；future 收集和分配成本须注明。做预热和多轮测量，报告中位数与波动范围，包含小任务开销和较粗 CPU 任务两类负载，不承诺小任务加速。直接串行执行用于说明调度开销及并行收益，不宣称它是另一种线程池的性能对照。

当前没有基准程序或实测性能数据；本轮不生成性能数字，也不写未经测量的简历性能结论。

## 9. 本轮检查记录

初次检查覆盖 API.md、原 DESIGN.md、IMPLEMENTATION.md、thread_pool.hpp、thread_pool.cpp、两个测试和 CMake 配置。当前文档同步基于工作区已有版本，保留其已完成进度说明；头文件已有未提交改动，本轮不触碰源码、测试或构建配置。

上一轮精简前基线：ucrt64-debug 配置成功，构建通过，thread_pool_submit 与 thread_pool_worker 两项 CTest 均通过（2/2）。该结果只证明现有 Task/submit/真实 worker 与测试桩配合的行为，不证明本文的目标关闭契约。本轮仅做文档同步，不将旧测试结果视为目标行为已通过验证。

用户已确认精简方向与补充并发决定；本轮修订文档确认前不修改源码或测试。
