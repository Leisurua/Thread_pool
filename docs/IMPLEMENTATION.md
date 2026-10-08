# 实现进度与验证

[项目入口](../README.md) · [目标 API 契约](API.md) · [内部设计](DESIGN.md)

当前模块 1 至 4 的核心功能均已实现，完整公共接口可直接链接使用；模块 5 性能验证尚未开始，整体项目尚未达到包含可复现性能数据的完成标准。模块 1 至 3 已通过用户审核，提交分别为 b4340f5、68daae6 和 722e5fe；后续生命周期、关闭及 enqueue 拆分仍在工作区，未提交。审核记录见 [REVIEW.md](REVIEW.md)。

API.md 描述精简后的目标契约，DESIGN.md 描述目标内部结构；本文分别记录当前源码事实、目标差异与后续验证，避免将文档决定当作已实现行为。

代码采用 C++20 头文件与源文件分离结构，池构造/析构、shutdown/shutdown_now/stop、私有 enqueue 和 work_thread::run 在 src/thread_pool.cpp 中实现，其余已实现逻辑位于头文件。构造、submit、作用域析构及完整公共关闭均可直接链接使用。

## 1. 当前源码事实（模块 1 至 4 已实现，前三轮已通过用户审核）

| 模块 | 当前进度 | 与精简目标的关系 |
| --- | --- | --- |
| 状态与公共接口 | 四种运行/关闭状态；两种关闭及共用 stop 已实现；暂停恢复与快照 wait 已删除 | 状态与接口精简完成，公共生命周期模块 4 已完成 |
| Task 类型擦除 | 保留非模板外层、unique_ptr<base>、model<R> 和 packaged_task；已删除 ID、cancel 和取消标志 | 未执行 Task 的销毁反馈 broken_promise，架构精简完成 |
| submit / enqueue | submit 对外，锁外准备 Task/future；私有 enqueue(Task) 在 cpp 中负责锁内状态/有效性检查、入队及锁外通知 | 成功入队仍是接受边界；失败先解锁再清理 Task，原异常传播 |
| 提交与任务异常 | 已删除 submission_rejected、submission_error、task_cancelled、task_execution_error | 拒绝提交抛 runtime_error；准备/入队失败及任务异常传播原类型 |
| thread_pool_call_error | 已删除 | 关闭入口已使用 std::logic_error 拒绝本池 worker；负责人重入检查也已接入 stop |
| 默认 worker 数量 | 已实现，采用 hardware_concurrency()，返回 0 保底 1 | 保留，不重写 |
| thread_pool 构造与析构 | 已实现指定/默认数量创建、显式 0 拒绝、部分失败通知与回收、优雅析构及 worker 自析构 terminate | 默认数量直接沿用 default_worker_count()；析构复用 stop(SHUTDOWN)，完成后不重复 join |
| work_thread 生命周期与 run | 已实现 jthread 启动、谓词等待、FIFO 领取、锁外执行/销毁及退出；已删除暂停分支与 finish_task 登记 | 等待谓词为“非 RUNNING 或队列非空”，精简完成 |
| worker 身份判定 | 已实现 TLS、RAII 守卫与 is_current_worker | 保留；关闭入口已接入 worker 自关闭拒绝 |
| 关闭负责人重入检测 | 已实现 closing_thread_id_，持队列锁拒绝同线程重入 | 身份维持至丢弃、join 和发布 TERMINATED 完成，之后清空 |
| pause / resume / change_state | 已删除 | 不新增替代接口 |
| shutdown / shutdown_now / stop | 已实现双锁分工、状态转换、队列锁外丢弃/join、关闭锁等待及完成发布 | 实现首次关闭方式保持到底、唯一负责人 join、其他调用者等待 |
| ID / finish_task / 快照进度 | Task ID、接受编号、unfinished set、最早丢弃记录、完成登记及专用同步成员均已删除 | 使用点和 set/limits 依赖已清理，不新增产品进度计数 |
| wait / wait_result | 快照接口及结果类型均已删除 | 通过各任务 future 等待，不增加替代全池 wait |
| 性能数据 | 没有基准程序或实测结果 | 生命周期和正确性完成后单独测量 |

src/thread_pool.cpp 编译进 thread_pool 静态库；使用方须链接 thread_pool::thread_pool。stop 已有产品定义，生命周期/stop 定义桩均已删除，worker 测试使用产品关闭接口。submit 测试在产品构造后先回收 worker，再同步驱动队列。

## 2. 已确定的目标要求（按模块逐步落实）

- 保留固定 worker、无界 FIFO、泛型 submit、future、现有 Task 类型擦除、jthread 和 worker 身份检测。不引入新的任务包装机制或结果通道。
- callable/参数按值保存，callable 左值调用、参数右值传入；沿用 move-only、std::ref、值/void/左值引用结果及右值引用结果 static_assert。
- 仅 RUNNING 接受提交；队列锁内成功入队是接受边界。拒绝提交抛 std::runtime_error；准备、加锁和入队失败传播原异常，不包装。
- 用户任务异常由 packaged_task 保存，future.get() 按原类型抛出。丢弃尚未领取任务通过锁外销毁未执行的 packaged_task 反馈 broken_promise，不运行 callable。
- 优雅关闭继续领取并排空队列；立即关闭丢弃尚未领取任务，已领取任务继续执行；两种关闭均等待所有 worker join。
- 保留并发关闭支持：首次在队列锁内生效的关闭调用是唯一负责人，其他调用者等待 shutdown_mutex_ 时不持队列锁，不重复 join，不升级或降级关闭方式。
- 保留 worker TLS 自关闭检测；增加 queue_mutex_ 保护的 closing_thread_id_，在获取关闭锁前拒绝同线程负责人重入。显式关闭抛 std::logic_error，worker 或关闭负责人重入析构本池采用 std::terminate。
- 负责人身份维持至丢弃清理、join 和发布 TERMINATED 完成；清空 ID 与状态发布在队列锁内完成，再释放关闭锁，使其他关闭调用者可以继续。不保留 join_in_progress_ 或增加另一套 join 资格。
- 用户任务执行、捕获对象销毁、未执行 packaged_task 的销毁反馈与 worker join 均在队列锁外；条件变量等待释放队列锁。
- 构造部分失败时先设置退出状态、通知 worker，再在队列锁外回收线程，最后传播原异常。jthread 的 stop 请求不替代池状态/通知协议。
- 析构前调用方协调全部外部访问；worker 集合保留最后声明，回收先于同步依赖销毁。future 可能延长捕获对象生命周期。
- 删除暂停、快照 wait、ID、历史取消、取消标志、自定义异常和关闭升级，不新增公共 API。动态 worker、有界队列、任务窃取和优先级调度不进入此次范围。

负责人重入检测只处理同线程关闭本池的自等待，不保证消除跨线程或跨池等待环。用户析构中若触发重入拒绝或关闭期间提交拒绝，须自行捕获，避免异常逃逸 noexcept 析构。

## 3. 分模块实施顺序

模块 1 至 3 已完成并通过用户审核。模块 4 分为构造/优雅析构和显式关闭两步：两步均已完成，析构已统一调用 stop；性能验证尚未开始。每轮验证后停止。新的并发问题先说明原因与可选方案，再讨论决定。

| 次序 | 功能模块 | 直接修改范围 | 主要验证 |
| --- | --- | --- | --- |
| 1（已完成） | 任务结果与异常反馈 | 保留 Task/base/model/packaged_task；移除自定义异常、取消标志和 cancel；适配 submit 与 worker 测试反馈 | 原异常类型/信息、用户复制失败原异常、结果存储失败、销毁未执行 Task 后 broken_promise、callable 不被调用、异常后 worker 继续运行 |
| 2（已完成） | 移除快照进度 | 删除 wait/wait_result、Task ID、接受编号、set、历史取消、finish_task、progress_changed_、wait_active_；同步去掉 submit/worker 登记 | 保留多生产者恰好执行一次、锁外准备、FIFO；删除 ID 溢出/历史取消断言，队列分配失败注入已独立于 set |
| 3（已完成，审核通过） | 移除暂停恢复 | 删除 pause/resume、PAUSED、change_state；简化 submit 状态检查和 worker 谓词 | 受控阻塞任务/latch 替代暂停桩组织 FIFO、已领取与未领取边界；空队列通知、后续提交和退出测试通过 |
| 4a（已完成） | 池构造与优雅析构 | 实现构造/失败回收/析构；删除测试构造/析构定义桩；新增生命周期测试 | 默认/指定数量、显式 0、直接链接使用、析构排空与等待、首个 worker 创建后分配失败回收和重试 |
| 4b（已完成） | 两种显式关闭 | shutdown/shutdown_now/stop 已实现；加入 shutdown_mutex_ 串行化关闭，原 mutex_ 改名为 queue_mutex_；删除 shutdown_complete_，保留负责人线程 ID；join_in_progress_ 已删除，析构复用关闭流程 | 公共关闭、立即丢弃、并发关闭与无升级、worker 自关闭、负责人重入、已关闭后析构 |
| 5（未开始） | 性能验证 | 单独建立最小基准、记录环境/计时边界/结果，随后更新展示文档 | 校验任务结果；测量提交开销及完整批次吞吐，比较直接串行、单 worker 和多 worker，多轮数据 |

两个旧测试的同名构造/析构定义桩已删除，所有测试链接产品生命周期；worker 已使用产品关闭接口；submit 仍保留同步驱动 Task 的友元状态夹具。

不新增全池完成计数、原子进度变量或通用统计系统。删除快照进度后，通过任务 future 和 worker join 验证对应完成边界。

## 4. 构建目标与运行方式（保持现状）

| 目标 | 当前作用 |
| --- | --- |
| thread_pool / thread_pool::thread_pool | STATIC 库，编译池构造/析构、shutdown/shutdown_now/stop、enqueue 及 worker 循环，传递包含目录、C++20、线程依赖和警告选项 |
| thread_pool_header_check | OBJECT 目标，验证头文件独立编译与重复包含，不链接完整线程池 |
| thread_pool_submit_test | 产品构造后回收 worker，以状态驱动同步验证 submit 与 Task |
| thread_pool_worker_test | 产品构造/析构与真实 work_thread::run，使用产品关闭接口验证任务排空、立即丢弃及 worker 退出 |
| thread_pool_lifecycle_test | 直接验证产品构造、优雅析构及部分创建失败回收，验证真实并发关闭、无升级、负责人重入、关闭准备失败回滚及完成后析构 |

CMake 使用 include(CTest)，默认启用 BUILD_TESTING；可通过 -DBUILD_TESTING=OFF 关闭行为测试目标。CTest 名称为 thread_pool_submit、thread_pool_worker 和 thread_pool_lifecycle，各自超时 30 秒。

测试友元访问器与全局分配失败注入分别属于独立可执行文件，不合并链接或合入产品库。所有 stop 定义桩已删除，全部行为测试链接产品关闭流程。

在项目根目录执行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
ctest --test-dir build/ucrt64-debug --output-on-failure
```

产物位于 build/ucrt64-debug/。使用 MSYS2 UCRT64 GNU GCC 16.2.0、CMake + Ninja、C++20，禁用 GNU 扩展；预设只为项目构建设置 PATH。工具链及 clangd 设置见 [CLAUDE.md](../CLAUDE.md)。

## 5. 现有测试覆盖（产品生命周期及显式关闭已接入）

[submit_test.cpp](../tests/submit_test.cpp) 产品构造后先回收 worker，再通过状态驱动与友元访问同步测试 submit 与 Task，已覆盖：

- submit 返回时任务尚未执行，future 尚未就绪。
- 值、void、左值引用结果，move-only callable/参数、std::ref 和仅左值可调用对象。
- 原函数异常的动态类型和信息直接保留，packaged_task 内部结果存储异常保持原类型。
- 未执行的值/void/左值引用 Task 在锁外销毁后，future 就绪并反馈 broken_promise，callable 不执行。
- 仅 RUNNING 接受提交，三种关闭状态拒绝且不入队。
- 私有 enqueue 拒绝空/已移动 Task，不入队；有效 Task 可直接入队并通过原 future 取值。被拒绝任务的捕获析构可再次 submit 并获得拒绝，验证 enqueue 的异常清理在锁外。
- 用户复制构造抛 runtime_error 派生异常时，原动态类型与信息保持不变。
- 受控复制阻塞时另一个 submit 仍可入队并执行，验证锁外准备。
- 多线程提交后逐任务核对结果与调用次数，验证恰好执行一次。
- 同一 callable 的多次成功 submit 用于测量准备分配数，跳过准备分配后对额外的队列扩容分配注入失败；bad_alloc 直接传播，已有任务保持原状且可重试。该注入针对本机 UCRT64 标准库，不依赖产品 ID/set 或固定对象大小。

[worker_test.cpp](../tests/worker_test.cpp) 使用产品池生命周期创建真实 worker，通过产品关闭接口验证 worker 行为，已覆盖：

- 本池/池外/其他池线程身份判定，任务内 submit 可继续执行。
- 任务异常按原动态类型与信息进入 future，之后任务继续执行。
- 唯一 worker 被 latch 阻塞时，后续任务保持排队且 future 未就绪；解除阻塞后按 FIFO 执行。
- 双 worker 乱序完成时，后一个 future 先就绪，受 latch 阻塞的前一个 future 仍未就绪。
- 四个生产者和四个真实 worker 并发提交/领取 96 个任务，核对结果，join 后逐任务核对执行次数均为 1；计数仅用于测试，不加入产品。
- worker 持有最后一个共享状态引用时，Task 释放触发的用户析构可 submit 到本池；析构通过 latch 通知主线程，验证销毁在锁外并同步结果写入。
- 优雅关闭排空剩余队列；立即关闭保留已领取阻塞任务、丢弃其余任务，future 反馈 broken_promise 后解除已领取任务阻塞。
- 未执行 Task 持有最后一个共享状态引用时，锁外丢弃触发的用户析构可再次 submit，不持有队列锁。
- 空队列通知不改变运行状态，后续提交仍能执行；两种产品关闭接口均使空闲 worker 退出，完成后重复调用直接返回。单独的队列丢弃友元夹具仅用于运行状态下的锁外析构测试。

测试已移除暂停桩、暂停断言、ID 溢出、历史取消和快照登记断言，以及相关等待驱动。任务结果通过 future 验证；需要确认捕获对象销毁时使用 latch，需要确认 worker 退出时使用 join，不能将 future 就绪视为这两个边界。

[lifecycle_test.cpp](../tests/lifecycle_test.cpp) 新增验证：指定和默认 worker 数量、显式 0 拒绝、产品接口直接链接与 submit、析构等待已领取阻塞任务并排空后续 8 个任务、future 全部就绪且逐任务只执行一次；本机 UCRT64 下在第二个 work_thread 对象分配时注入 bad_alloc，验证首个 worker 回收、原异常传播及重新构造成功。注入依赖本机对象分配大小，不增加产品测试接缝，也不耗尽系统线程资源；不等同于操作系统线程创建失败注入。

生命周期测试补充：真实 worker 经两个公共入口及直接 stop 自关闭均抛 logic_error，池仍可执行任务；两种首次方式各以一个负责人和 8 个混合接口调用者验证等待及无升级；优雅队列逐任务执行一次，立即队列不执行且 future 反馈 broken_promise，已领取任务继续完成；完成后 worker 集合为空、负责人 ID 清空且重复关闭正常返回。丢弃捕获析构重入两个关闭接口得到 logic_error，submit 得到拒绝，同一线程关闭另一池正常；局部丢弃队列首次分配 bad_alloc 时状态/负责人未改变，已有任务仍可完成并可重试关闭。

并发关闭、无升级、负责人重入及显式关闭后析构均已通过真实接口验证。worker 和负责人重入析构的 terminate 路径已接入，尚无子进程测试，不将当前行为测试视为 terminate 路径验证。

## 6. 生命周期验证边界与后续补充

以下行为已由现有测试覆盖，使用 latch、promise/future 与受控任务建立确定顺序，超时用于防止挂死，不用 sleep 推断关闭已生效。尚未验证的路径单独列在本节末尾。

- 优雅关闭：阻塞任务解除后，所有已接受任务恰好执行一次，future 可读，空闲和忙碌 worker 均被 join。
- 立即关闭：先确保部分任务已领取，其余任务排队；关闭生效后排队 callable 不运行，future 报 broken_promise，已领取任务正常完成。
- 并发/重复关闭：多个调用者均等同一次关闭完成，只有首次关闭者 join；混用两种接口不改变首次方式。用受控同步确认首次方式已生效，再启动后续调用。
- worker 自关闭：两种接口均在修改状态和等待前抛 logic_error，未捕获错误经原类型 future 反馈，不妨碍后续任务。
- 负责人重入：让捕获对象在关闭线程清理未执行 Task 时析构，在析构内分别调用两种关闭接口并捕获 logic_error；外层关闭完成、其他调用者获通知，方式与进度不被改变。控制最后一个共享状态引用，避免 future 延长捕获生命周期而绕过目标路径。
- 身份范围：只拒绝同线程正在关闭同一池的重入；其他池不应因身份匹配错误被拒绝。验证完成后负责人 ID 清空，后续顺序关闭正常返回。
- 构造/析构：显式 0、默认数量规则、析构优雅等待与已关闭后不重复 join；通过第二个 work_thread 对象分配失败验证部分构造回收，不增加产品接缝或耗尽系统线程资源。
- 锁边界：任务内 submit、锁外捕获析构，以及立即关闭清理时用户析构进入池接口获得拒绝/重入反馈。不能让异常逃逸析构。

尚未覆盖：worker 和负责人重入析构本池的 terminate 子进程测试、操作系统线程创建失败注入，以及 hardware_concurrency() 返回 0 的受控路径。这些逻辑已在源码中实现，当前测试通过不表示上述路径已被实际触发。terminate 测试须隔离在独立进程，不能结束其余测试。

## 7. 性能与验证记录

### 当前核对（2026-10-08）

已逐项对照头文件、源文件、CMake 和三份测试：核心功能模块 1 至 4 均有产品实现，无生命周期/stop 定义桩；submit 测试仍使用友元状态夹具。ucrt64-debug 配置、构建（含头文件独立/重复包含检查）及三项 CTest（3/3）通过。本次仅同步文档，未改变产品代码。性能基准及实测数据仍为空缺。

### 待执行的性能验证

正确性与生命周期完成后使用现有 ucrt64-release 预设，记录 CPU/OS/编译器、worker 和生产者数量、任务负载、任务数及计时边界。测试相同计算工作，包含小任务调度开销与较粗 CPU 任务；结果使用可观测校验值。

比较直接串行、单 worker、多 worker；分别报告批量 submit 的平均提交耗时，以及首个提交至全部 future.get 完成的总耗时和吞吐，注明 future 收集与分配成本。预热、多轮测量，报告中位数与波动范围；不承诺小任务加速，也不编造性能数据。

### 历史实施记录

以下按实施顺序保留当时状态；其中的“本轮”“待实现”和测试桩描述不代表当前源码，当前状态见第 1、5 节及本节当前核对。

精简前基线已保存为 master 上的本地提交 d0445e9；精简分支为 refactor/simplify-thread-pool。模块 1 提交为 b4340f5，构建及两项 CTest 通过（2/2）。

模块 2 提交为 68daae6，wait/ID/set/finish_task 及其专用依赖已全部移除；配置、构建及两项 CTest 通过（2/2）。

本轮完成模块 3，删除暂停恢复接口、PAUSED 和 change_state，简化 submit 与 worker 状态谓词。测试桩初始状态为 RUNNING，使用 latch 控制排队和已领取任务；保留生命周期桩、shutdown_complete_ 及待生命周期模块处理的 join_in_progress_。

第三轮与审核后的 hpp 顺序整理已保存为 722e5fe，构建及两项 CTest 通过（2/2）。

本轮按用户要求仅实现构造和优雅析构，close 留待后续。构造预留 worker 容器容量，避免启动线程后扩容失败导致临时 worker 阻塞回收；部分失败先设置退出状态并通知，再锁外回收。析构使用状态/通知/锁外回收协议，不调用尚未实现的 close。ucrt64-debug 配置、构建（含头文件检查）、三项 CTest（3/3）及 Git 差异格式检查通过。本轮验证后停止。

随后按用户要求将 submit 内排队职责提取为私有 enqueue(Task)，在 cpp 中实现；submit 仅准备任务和结果通道并返回 future。enqueue 检查接受状态、Task 有效性，安全 push 后解锁通知，保留失败时锁外清理。新增空 Task 拒绝与拒绝清理重入验证，配置、构建（含头文件检查）、三项 CTest（3/3）及 Git 差异格式检查通过。构造/析构及此次职责拆分仍保留在工作区，未提交。

按用户要求删除未使用的 join_in_progress_ 成员及构造初始化，同步清理主锁注释；关闭负责人机制仍留待 close 模块实现，不新增替代布尔值。

本轮按用户要求先实现 shutdown/shutdown_now 入口框架，将私有 close(status_t) 改名为 stop(status_t)。入口先拒绝本池 worker 自关闭，再持锁检查 TERMINATED；其余情况解锁传入各自目标状态。stop 保留声明，状态转换、丢弃、并发等待与 join 尚待后续；内联入口使现有构造/submit/析构仍可独立链接使用。

本轮 ucrt64-debug 配置、构建（含头文件检查）、三项 CTest（3/3）及 Git 差异格式检查通过。生命周期测试使用仅记录调用的 stop 桩验证入口框架；改动未提交。

随后按用户要求将 shutdown/shutdown_now 从头文件内联定义移至 src/thread_pool.cpp，头文件仅保留声明，入口行为保持不变。由于共用 stop 尚未实现，submit/worker 测试补充仅用于链接且意外调用即 terminate 的测试桩；生命周期测试继续使用调用记录桩。产品库未添加占位关闭实现，完整链接须待 stop 实现。

移动后 ucrt64-debug 配置、构建、三项 CTest（3/3）及 Git 差异格式检查通过，改动未提交。

本轮在 src/thread_pool.cpp 实现 stop：现有 mutex_ 保护状态、队列和负责人 ID，首次状态转换确定唯一负责人，其余调用者在 shutdown_complete_ 上释放主锁等待。立即关闭在改变状态前准备局部队列，锁内交换、锁外销毁；两种方式均锁外 join，最后持锁发布 TERMINATED、清空负责人并通知等待者。析构复用 stop，删除所有 stop 定义桩，worker 关闭测试改为产品接口。新增并发混用、无升级、负责人重入、身份范围及关闭准备失败验证。ucrt64-debug 配置、构建、三项 CTest（3/3）通过；改动未提交。

按用户要求改为两把锁：shutdown_mutex_ 串行化整个关闭、丢弃及 worker 回收；queue_mutex_ 保护状态、队列和负责人身份，submit/worker 只使用队列锁。stop 在获取关闭锁之前以队列锁预检查负责人重入，检查结束释放队列锁；同时持锁时按关闭锁到队列锁顺序获取，丢弃/join 时仅持关闭锁。移除被关闭锁等待替代的 shutdown_complete_，保留首次方式与自等待拒绝语义。现有真实并发关闭、锁外析构重入、分配失败及任务行为测试均通过；ucrt64-debug 配置、构建、三项 CTest（3/3）通过，改动未提交。

本轮按用户补充进一步明确锁职责：queue_mutex_ 维护原有入队、领取、移出队列及状态检查/变更顺序；shutdown_mutex_ 只互斥并发关闭调用，不进入 submit/worker 任务同步。现有实现已符合此分工，本轮仅调整注释与职责说明，运行逻辑未改变，未重复运行行为测试，改动未提交。

用户要求提交到本地。本次提交纳入构造/优雅析构、submit/enqueue 拆分、两种关闭及双锁职责划分、对应真实接口测试与文档；提交前再次完成 ucrt64-debug 配置、构建、三项 CTest（3/3）及差异格式检查，保留上述分阶段记录。提交位于 refactor/simplify-thread-pool 本地分支。
