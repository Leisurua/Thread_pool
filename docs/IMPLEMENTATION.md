# 实现进度与验证

[项目入口](../README.md) · [目标 API 契约](API.md) · [内部设计](DESIGN.md)

精简方向及用户补充决定已确定，现已完成模块 1“任务结果与异常反馈”和模块 2“移除快照进度”。每轮仅修改一个明确功能模块及其直接依赖的测试，验证后停止；下一模块为移除暂停恢复。

API.md 描述精简后的目标契约，DESIGN.md 描述目标内部结构；本文分别记录当前源码事实、目标差异与后续验证，避免将文档决定当作已实现行为。

代码采用 C++20 头文件与源文件分离结构，work_thread::run 在 src/thread_pool.cpp 中实现，其余已实现逻辑位于头文件。当前完整线程池尚不能直接使用。

## 1. 当前源码事实（模块 1、2 完成）

| 模块 | 当前进度 | 与精简目标的关系 |
| --- | --- | --- |
| 状态与公共接口 | 五种状态，含 PAUSED；暂停恢复与两种关闭均有声明；快照 wait 已删除 | PAUSED 和 pause/resume 留待模块 3 删除；保留四种运行/关闭状态 |
| Task 类型擦除 | 保留非模板外层、unique_ptr<base>、model<R> 和 packaged_task；已删除 ID、cancel 和取消标志 | 未执行 Task 的销毁反馈 broken_promise，架构精简完成 |
| submit | 保留锁外准备、状态检查、直接入队及 future 返回；已删除编号/set 登记、回滚与异常包装 | 成功入队仍是接受边界；PAUSED 接受行为留待模块 3 删除 |
| 提交与任务异常 | 已删除 submission_rejected、submission_error、task_cancelled、task_execution_error | 拒绝提交抛 runtime_error；准备/入队失败及任务异常传播原类型 |
| thread_pool_call_error | 已删除 | 显式自等待错误将使用 std::logic_error，公共关闭检查尚未接入 |
| 默认 worker 数量 | 已实现，采用 hardware_concurrency()，返回 0 保底 1 | 保留，不重写 |
| thread_pool 构造与析构 | 仅声明 | worker 创建、部分失败回收、默认优雅关闭待实现 |
| work_thread 生命周期与 run | 已实现 jthread 启动、谓词等待、FIFO 领取、锁外执行/销毁及退出；已删除 finish_task 登记 | 保留架构，暂停分支留待模块 3 删除 |
| worker 身份判定 | 已实现 TLS、RAII 守卫与 is_current_worker | 保留；关闭入口拒绝检查尚未接入 |
| 关闭负责人重入检测 | 未实现，当前没有负责人线程 ID | 增加一个私有 std::thread::id，由现有 mutex_ 保护 |
| pause / resume / change_state | 仅声明 | 删除，不实现 |
| shutdown / shutdown_now / close | 仅声明，旧注释涉及升级与共用 join | 实现首次关闭方式保持到底、唯一负责人 join、其他调用者等待 |
| ID / finish_task / 快照进度 | Task ID、接受编号、unfinished set、最早丢弃记录、完成登记及专用同步成员均已删除 | 使用点和 set/limits 依赖已清理，不新增产品进度计数 |
| wait / wait_result | 快照接口及结果类型均已删除 | 通过各任务 future 等待，不增加替代全池 wait |
| 性能数据 | 没有基准程序或实测结果 | 生命周期和正确性完成后单独测量 |

src/thread_pool.cpp 编译进 thread_pool 静态库；使用方须链接 thread_pool::thread_pool。测试中的生命周期和状态桩不是产品实现，静态库及测试可构建不代表公共池构造和关闭已可使用。

## 2. 已确定的目标要求（按模块逐步落实）

- 保留固定 worker、无界 FIFO、泛型 submit、future、现有 Task 类型擦除、jthread 和 worker 身份检测。不引入新的任务包装机制或结果通道。
- callable/参数按值保存，callable 左值调用、参数右值传入；沿用 move-only、std::ref、值/void/左值引用结果及右值引用结果 static_assert。
- 仅 RUNNING 接受提交；主锁内成功入队是接受边界。拒绝提交抛 std::runtime_error；准备、加锁和入队失败传播原异常，不包装。
- 用户任务异常由 packaged_task 保存，future.get() 按原类型抛出。丢弃尚未领取任务通过锁外销毁未执行的 packaged_task 反馈 broken_promise，不运行 callable。
- 优雅关闭继续领取并排空队列；立即关闭丢弃尚未领取任务，已领取任务继续执行；两种关闭均等待所有 worker join。
- 保留并发关闭支持：首次在主锁内生效的关闭调用是唯一负责人，其他调用者等待 shutdown_complete_，不重复 join，不升级或降级关闭方式。
- 保留 worker TLS 自关闭检测；增加 mutex_ 保护的 closing_thread_id_，在进入等待前拒绝同线程负责人重入。显式关闭抛 std::logic_error，worker 或关闭负责人重入析构本池采用 std::terminate。
- 负责人身份维持至丢弃清理、join 和发布 TERMINATED 完成；清空 ID 与状态发布在主锁内完成，再通知等待者。不保留 join_in_progress_ 或增加另一套 join 资格。
- 用户任务执行、捕获对象销毁、未执行 packaged_task 的销毁反馈与 worker join 均在主锁外；条件变量等待释放主锁。
- 构造部分失败时先设置退出状态、通知 worker，再在主锁外回收线程，最后传播原异常。jthread 的 stop 请求不替代池状态/通知协议。
- 析构前调用方协调全部外部访问；worker 集合保留最后声明，回收先于同步依赖销毁。future 可能延长捕获对象生命周期。
- 删除暂停、快照 wait、ID、历史取消、取消标志、自定义异常和关闭升级，不新增公共 API。动态 worker、有界队列、任务窃取和优先级调度不进入此次范围。

负责人重入检测只处理同线程关闭本池的自等待，不保证消除跨线程或跨池等待环。用户析构中若触发重入拒绝或关闭期间提交拒绝，须自行捕获，避免异常逃逸 noexcept 析构。

## 3. 分模块实施顺序

模块 1、2 已完成并通过 submit 与 worker 测试；模块 3 至 5 尚未开始。每个模块落实后同步文档中的实际进度，运行相应测试，不自动串行完成全部模块。新的并发问题先说明原因与可选方案，再讨论决定。

| 次序 | 功能模块 | 直接修改范围 | 主要验证 |
| --- | --- | --- | --- |
| 1（已完成） | 任务结果与异常反馈 | 保留 Task/base/model/packaged_task；移除自定义异常、取消标志和 cancel；适配 submit 与 worker 测试反馈 | 原异常类型/信息、用户复制失败原异常、结果存储失败、销毁未执行 Task 后 broken_promise、callable 不被调用、异常后 worker 继续运行 |
| 2（已完成） | 移除快照进度 | 删除 wait/wait_result、Task ID、接受编号、set、历史取消、finish_task、progress_changed_、wait_active_；同步去掉 submit/worker 登记 | 保留多生产者恰好执行一次、锁外准备、FIFO；删除 ID 溢出/历史取消断言，队列分配失败注入已独立于 set |
| 3 | 移除暂停恢复 | 删除 pause/resume、PAUSED、change_state；简化 submit 状态检查和 worker 谓词 | 用受控阻塞任务/latch 替代暂停桩组织 FIFO、已领取与未领取边界，验证空队列唤醒和退出 |
| 4 | 池生命周期与两种关闭 | 实现构造/失败回收/析构/close；保留 shutdown_complete_，移除 join_in_progress_，增加负责人线程 ID；删除测试生命周期定义桩 | 公共接口直接链接使用、优雅排空、立即丢弃、并发关闭与无升级、worker 自关闭、负责人重入、析构等待、部分构造失败回收 |
| 5 | 性能验证 | 单独建立最小基准、记录环境/计时边界/结果，随后更新展示文档 | 校验任务结果；测量提交开销及完整批次吞吐，比较直接串行、单 worker 和多 worker，多轮数据 |

模块 1 至 3 仍可使用生命周期桩，但需保留显著说明。模块 4 提供产品构造/析构定义时，须同步删除两个测试各自的同名定义，避免重复符号和绕过产品接口。

不新增全池完成计数、原子进度变量或通用统计系统。删除快照进度后，通过任务 future 和 worker join 验证对应完成边界。

## 4. 构建目标与运行方式（保持现状）

| 目标 | 当前作用 |
| --- | --- |
| thread_pool / thread_pool::thread_pool | STATIC 库，编译 worker 循环并传递包含目录、C++20、线程依赖和警告选项 |
| thread_pool_header_check | OBJECT 目标，验证头文件独立编译与重复包含，不链接完整线程池 |
| thread_pool_submit_test | 使用生命周期桩验证产品 submit 与 Task |
| thread_pool_worker_test | 使用生命周期/状态桩验证真实 work_thread::run，以 future、latch 和 join 判断对应完成边界 |

CMake 使用 include(CTest)，默认启用 BUILD_TESTING；可通过 -DBUILD_TESTING=OFF 关闭行为测试目标。CTest 名称为 thread_pool_submit 和 thread_pool_worker，各自超时 30 秒。

两个测试可执行文件分别定义自己的池生命周期桩，不应合并链接或将桩合入产品库。本轮不改变构建配置和测试目标。

在项目根目录执行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --test-dir build/ucrt64-debug --output-on-failure
```

产物位于 build/ucrt64-debug/。使用 MSYS2 UCRT64 GNU GCC 16.2.0、CMake + Ninja、C++20，禁用 GNU 扩展；预设只为项目构建设置 PATH。工具链及 clangd 设置见 [CLAUDE.md](../CLAUDE.md)。

## 5. 现有测试覆盖（模块 1、2 已适配，暂停及生命周期待迁移）

[submit_test.cpp](../tests/submit_test.cpp) 不创建真实 worker，通过生命周期桩和友元访问直接驱动产品 submit 与 Task，已覆盖：

- submit 返回时任务尚未执行，future 尚未就绪。
- 值、void、左值引用结果，move-only callable/参数、std::ref 和仅左值可调用对象。
- 原函数异常的动态类型和信息直接保留，packaged_task 内部结果存储异常保持原类型。
- 未执行的值/void/左值引用 Task 在锁外销毁后，future 就绪并反馈 broken_promise，callable 不执行。
- PAUSED 接受提交，三种关闭状态拒绝且不入队。
- 用户复制构造抛 runtime_error 派生异常时，原动态类型与信息保持不变。
- 受控复制阻塞时另一个 submit 仍可入队并执行，验证锁外准备。
- 多线程提交后逐任务核对结果与调用次数，验证恰好执行一次。
- 同一 callable 的多次成功 submit 用于测量准备分配数，跳过准备分配后对额外的队列扩容分配注入失败；bad_alloc 直接传播，已有任务保持原状且可重试。该注入针对本机 UCRT64 标准库，不依赖产品 ID/set 或固定对象大小。

[worker_test.cpp](../tests/worker_test.cpp) 通过池生命周期与状态桩创建真实产品 worker，已覆盖：

- 本池/池外/其他池线程身份判定，任务内 submit 可继续执行。
- 任务异常按原动态类型与信息进入 future，之后任务继续执行。
- 暂停时不领取，恢复后单 worker FIFO。
- 双 worker 乱序完成时，后一个 future 先就绪，受 latch 阻塞的前一个 future 仍未就绪。
- 四个生产者和四个真实 worker 并发提交/领取 96 个任务，核对结果，join 后逐任务核对执行次数均为 1；计数仅用于测试，不加入产品。
- worker 持有最后一个共享状态引用时，Task 释放触发的用户析构可 submit 到本池；析构通过 latch 通知主线程，验证销毁在锁外并同步结果写入。
- 直接设置优雅关闭状态后排空队列；直接设置立即关闭状态后保留已领取任务，不领取剩余任务。
- 未执行 Task 持有最后一个共享状态引用时，锁外丢弃触发的用户析构可再次 submit，不持有主锁。
- 关闭状态使空闲 worker 退出；测试桩将队列在锁内交换、锁外销毁，排队 Task 反馈 broken_promise。

测试已移除 ID 溢出、历史取消和快照登记断言，以及相关等待驱动。任务结果通过 future 验证；需要确认捕获对象销毁时使用 latch，需要确认 worker 退出时使用 join，不能将 future 就绪视为这两个边界。

异常替换与 Task 销毁反馈已经验证，但公共并发关闭、无升级、负责人重入检测、构造失败清理或产品析构仍未验证。直接驱动状态不是公共关闭接口测试。

## 6. 后续生命周期验证重点

使用 latch、promise/future 与受控任务建立确定顺序，超时用于防止挂死，不用 sleep 推断关闭已生效。

- 优雅关闭：阻塞任务解除后，所有已接受任务恰好执行一次，future 可读，空闲和忙碌 worker 均被 join。
- 立即关闭：先确保部分任务已领取，其余任务排队；关闭生效后排队 callable 不运行，future 报 broken_promise，已领取任务正常完成。
- 并发/重复关闭：多个调用者均等同一次关闭完成，只有首次关闭者 join；混用两种接口不改变首次方式。用受控同步确认首次方式已生效，再启动后续调用。
- worker 自关闭：两种接口均在修改状态和等待前抛 logic_error，未捕获错误经原类型 future 反馈，不妨碍后续任务。
- 负责人重入：让捕获对象在关闭线程清理未执行 Task 时析构，在析构内分别调用两种关闭接口并捕获 logic_error；外层关闭完成、其他调用者获通知，方式与进度不被改变。控制最后一个共享状态引用，避免 future 延长捕获生命周期而绕过目标路径。
- 身份范围：只拒绝同线程正在关闭同一池的重入；其他池不应因身份匹配错误被拒绝。验证完成后负责人 ID 清空，后续顺序关闭正常返回。
- 构造/析构：显式 0、默认数保底、析构优雅等待与已关闭后不重复 join；部分线程创建失败通过可控测试注入验证回收，不靠耗尽系统线程资源。具体测试接缝在生命周期模块内决定。
- 锁边界：保留任务内 submit 和锁外捕获析构测试；新增立即关闭清理时用户析构可进入池接口并获拒绝/重入反馈的测试。不能让异常逃逸析构。

worker 和负责人重入析构本池的 terminate 路径若增加测试，应隔离在独立进程，不能结束其余测试；实现方式在生命周期模块中决定。

## 7. 性能与本轮验证记录

正确性与生命周期完成后使用现有 ucrt64-release 预设，记录 CPU/OS/编译器、worker 和生产者数量、任务负载、任务数及计时边界。测试相同计算工作，包含小任务调度开销与较粗 CPU 任务；结果使用可观测校验值。

比较直接串行、单 worker、多 worker；分别报告批量 submit 的平均提交耗时，以及首个提交至全部 future.get 完成的总耗时和吞吐，注明 future 收集与分配成本。预热、多轮测量，报告中位数与波动范围；不承诺小任务加速，也不编造性能数据。

精简前基线已保存为 master 上的本地提交 d0445e9；精简分支为 refactor/simplify-thread-pool。模块 1 提交为 b4340f5，构建及两项 CTest 通过（2/2）。

模块 2 修改头文件、worker 源文件及两份行为测试，同步设计、API、进度和项目入口说明。wait/ID/set/finish_task 及其专用依赖已全部移除；保留暂停行为、关闭同步成员和生命周期桩。

本轮 ucrt64-debug 配置、构建（含头文件检查）及 thread_pool_submit、thread_pool_worker 两项 CTest 均通过（2/2）；源码与测试中无快照依赖残留，Git 差异格式检查通过。本轮到此停止，下一模块移除暂停恢复；公共池生命周期仍未完成。
