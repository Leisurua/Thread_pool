# 实现进度与验证

[项目入口](../README.md) · [API 契约](API.md) · [内部设计](DESIGN.md) · [性能记录](PERFORMANCE.md)

核心功能和本机 Release 性能验证已完成。当前有 37 项 CTest；基础性能组 42 个配置、扩展组 84 个配置，各保存 10 次正式测量、预热、原始数据、环境及波动统计。本文记录源码状态、测试覆盖及历史过程；调用契约见 API.md，同步算法见 DESIGN.md。

## 1. 当前实现

| 组成 | 实现与边界 |
| --- | --- |
| 状态与接口 | RUNNING、SHUTDOWN、SHUTDOWN_NOW、TERMINATED；构造/析构、submit、两种关闭均可直接链接使用 |
| Task | 非模板外层、unique_ptr<base>、model<R> 和 packaged_task；只执行一次或直接销毁 |
| submit / enqueue | submit 锁外准备；私有 enqueue 锁内检查/入队，解锁后通知；成功入队即接受，失败传播原异常 |
| future 与异常 | 原任务异常及结果存储异常经 future 反馈；未领取任务丢弃反馈 broken_promise |
| worker | jthread、谓词等待、FIFO 领取、队列锁外执行/释放 Task；TLS 与 RAII 判断所属池 |
| 生命周期 | 指定/默认数量、显式 0 拒绝、部分创建失败回收；析构默认优雅关闭 |
| 关闭 | 关闭锁串行化丢弃、join 与完成发布；队列锁保护任务/状态顺序及负责人；首次方式保持到底 |
| 自等待检测 | worker 自关闭与同线程负责人重入抛 logic_error；重入析构导致 terminate |
| 性能基准 | 独立 Release 目标及运行/汇总脚本，不加入 CTest，不增加产品统计接口 |

池构造/析构、shutdown/shutdown_now/stop、enqueue 和 work_thread::run 位于 src/thread_pool.cpp，其余实现位于头文件。CMake 构建静态库，使用方须链接 thread_pool::thread_pool。

暂停恢复、快照 wait、任务 ID、完成集合、取消标志、自定义异常包装及关闭升级均已删除。没有全池进度计数、有界队列、动态 worker、任务窃取或优先级接口。

## 2. 已完成模块与提交

| 模块 | 结果 | 记录 |
| --- | --- | --- |
| 1：任务结果与异常反馈 | 移除自定义异常、cancel 和取消标志，保留原异常与 broken_promise | b4340f5，用户审核通过 |
| 2：移除快照进度 | 删除 wait、ID、set、历史取消及完成登记 | 68daae6，用户审核通过 |
| 3：移除暂停恢复 | 删除 pause/resume、PAUSED、change_state，简化 worker 谓词 | 722e5fe，用户审核通过，包含 hpp 顺序整理 |
| 4：生命周期与关闭 | 构造/失败回收、优雅析构、enqueue 拆分、两种关闭及双锁分工 | 3b24ed7；后续测试扩展见第 5 节 |
| 5：性能验证 | 基础 42 组、扩展 84 组各十轮，本机 Release 记录已保存 | [PERFORMANCE.md](PERFORMANCE.md) |

前三轮审核历史见 [REVIEW.md](REVIEW.md)。后续每轮按项目约定同步进度并验证后停止；历史阶段中的“待实现”和测试桩描述见第 7 节，不代表当前状态。

## 3. 构建目标与运行方式

| 目标 | 作用 |
| --- | --- |
| thread_pool / thread_pool::thread_pool | 静态库；传递包含目录、C++20、线程依赖及警告选项 |
| thread_pool_header_check | OBJECT 目标；验证头文件独立编译与重复包含 |
| thread_pool_basic_execution_test | 基础执行 5 项，每项验证 1 和 4 worker |
| thread_pool_shutdown_test | 优雅关闭 6 项，每项验证 1 和 4 worker |
| thread_pool_shutdown_now_test | 立即关闭 6 项，每项验证 1 和 4 worker；领取竞态合计 200 轮、51,200 个任务 |
| thread_pool_close_race_test | 关闭竞争与自等待 6 项，每项验证 1 和 4 worker |
| thread_pool_lifecycle_test | 生命周期与异常安全 12 项 |
| thread_pool_submit_test | 回收 worker 后由友元夹具同步驱动 submit 与 Task |
| thread_pool_worker_test | 真实 worker 与产品关闭接口回归 |
| thread_pool_benchmark | 可选独立基准；Release 预设启用，拒绝 Debug 及少于十次正式测量 |

CTest 共 37 项，五个分组标签为 basic_execution、graceful_shutdown、immediate_shutdown、close_race 和 lifecycle，每项超时 30 秒。头文件检查属于构建目标，不计入这 37 项。默认启用 BUILD_TESTING，可用 -DBUILD_TESTING=OFF 关闭行为测试目标。

在项目根目录运行：

```powershell
cmake --preset ucrt64-debug
cmake --build --preset ucrt64-debug
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
ctest --test-dir build/ucrt64-debug --output-on-failure
# 单独运行生命周期分组；其他分组替换标签。
ctest --test-dir build/ucrt64-debug -L lifecycle --output-on-failure
```

产物位于 build/ucrt64-debug/，使用 Windows / MSYS2 UCRT64、GNU GCC 16.2.0、CMake + Ninja、C++20，禁用 GNU 扩展。CTest 独立运行不继承 configure/build 预设 PATH，上述设置仅影响当前会话。工具链和 clangd 见 [CLAUDE.md](../CLAUDE.md)，Release 基准命令见 [PERFORMANCE.md](PERFORMANCE.md)。

## 4. 测试组织与证据边界

基础执行、优雅关闭、立即关闭三组仅通过公共接口验证行为。关闭竞争组通过公共接口关闭，用只读友元观察状态/负责人，worker 集合仅在全部关闭调用完成后读取。生命周期及 worker 回归包含测试专用友元观察和失败注入。全部测试链接产品生命周期和关闭实现，无生命周期/stop 定义桩。

submit 回归先关闭并回收产品 worker，再由友元恢复 RUNNING、同步领取和执行 Task，隔离验证泛型提交和入队异常；该夹具不代表公共接口支持重启。其余公共行为分组验证真实 worker。

测试用 latch、promise/future 和受控任务建立先后关系，不用 sleep 推断关闭生效。关闭生效由受控的状态观察、提交拒绝或 broken_promise 确认；future 就绪与 worker 退出分别验证。超时用于防止挂死，不作为延迟性能数据。

友元访问器和全局分配失败注入位于独立测试可执行文件，不合并链接或进入产品库。submit/lifecycle 的替换 new/delete 使用 noinline，避免 Release 下测试替换分配器被内联后触发 mismatched-new-delete 警告。

## 5. 现有测试覆盖

[basic_execution_test.cpp](../tests/basic_execution_test.cpp) 仅使用公共接口和真实 worker，基础任务执行进度 **5/5**；每项分别验证 1 个和 4 个 worker：

| 项目 | 验证范围 | 结果 |
| --- | --- | --- |
| 单 worker、多 worker 提交任务 | latch 阻塞同批任务，核对不同 worker 线程 ID 数量等于配置数量，并确认在提交线程之外执行、后续提交可继续执行 | 通过 |
| 1000 个任务恰好执行一次 | 4 个生产者同时开始，各提交 250 个任务；逐 future 核对结果 ID，shutdown/join 后逐 ID 核对原子调用次数均为 1，无遗漏、无重复 | 通过 |
| future 返回值、void 和左值引用 | 编译期核对三种 future 类型，运行期核对值、void 副作用、引用地址及经引用修改原对象 | 通过 |
| move-only callable、参数及 std::ref | 非复制且仅左值可调用对象、unique_ptr 按值参数与转移后空指针、move-only lambda，以及 std::ref 借用非复制参数和 callable 后修改原对象 | 通过 |
| 任务抛异常后 worker 继续执行 | 值/void 任务异常的原动态类型和信息保持；异常反馈后继续提交成功，单 worker 额外核对抛异常与后续任务的线程 ID 相同 | 通过 |

调用次数和线程 ID 仅用于测试。基础测试不访问私有状态，不使用 sleep 推断并发顺序；每项由 CTest 独立运行并设置 30 秒超时。

[shutdown_test.cpp](../tests/shutdown_test.cpp) 仅使用公共接口和真实 worker，优雅关闭 shutdown() 进度 **6/6**；每项分别验证 1 个和 4 个 worker：

| 项目 | 验证范围 | 结果 |
| --- | --- | --- |
| 空队列关闭正常返回 | 从未提交任务的池调用 shutdown，空闲 worker 退出，调用正常返回 | 通过 |
| 排队任务全部执行完毕 | latch 占住全部 worker 后提交 1000 个任务，关闭生效时任务仍未就绪；释放 worker 后，关闭返回时所有 future 就绪、结果正确、逐任务只执行一次 | 通过 |
| 等待正在执行的任务结束 | 全部 worker 阻塞时关闭已生效但尚未返回；释放任务后正常返回，任务完成计数及 TLS 析构退出计数均等于 worker 数量 | 通过 |
| 关闭生效后拒绝 submit | 在任务仍阻塞且关闭未返回时连续 32 次 submit 均抛 runtime_error，完成关闭后仍拒绝；被拒绝 callable 执行次数为 0 | 通过 |
| 重复 shutdown 不重复 join | 首次关闭后连续调用 100 次均正常返回，任务完成/worker 退出次数不变，随后产品析构正常；另核对源码中的 TERMINATED 直接返回和 worker 集合清空路径 | 通过 |
| 多线程同时 shutdown 正常返回 | 8 个调用者经共同起跑信号并发调用 shutdown，任务未释放时均未返回；释放后全部调用正常返回、100 个排队任务恰好执行一次、worker 退出次数正确 | 通过 |

测试不访问私有状态：通过 submit 首次抛 runtime_error 观察关闭生效，竞态中成功接受的探测任务保存 future，并核对关闭返回后均已完成。探测采用 yield 与 5 秒截止期限，不使用 sleep 推断关闭状态；TLS 析构计数仅用于测试，在 join 完成后读取。重复关闭验证公共行为与线程退出次数，不直接计量内部 join 调用次数，其控制路径另由源码核对。

[shutdown_now_test.cpp](../tests/shutdown_now_test.cpp) 仅使用公共接口和真实 worker，立即关闭 shutdown_now() 进度 **6/6**；每项分别验证 1 个和 4 个 worker：

| 项目 | 验证范围 | 结果 |
| --- | --- | --- |
| 未领取任务丢弃且 callable 不执行 | latch 占住全部 worker 后提交 1000 个任务，立即关闭使全部任务丢弃；恢复已领取任务并 join 后，逐 ID 执行次数仍为 0 | 通过 |
| 丢弃 future 报 broken_promise | 值、void、左值引用及 move-only 值结果的 future.get 均抛 future_error，错误码严格等于 future_errc::broken_promise；反馈发生在已领取任务释放前 | 通过 |
| 已领取任务继续执行并返回结果 | 丢弃标记任务的 broken_promise 确认关闭已生效，再放行各 worker；全部已领取任务返回对应值且执行一次 | 通过 |
| 空闲 worker 及时退出 | 从未提交的池、执行后空闲的池均在 5 秒限时内关闭；4 worker 池保留一个阻塞任务时，其余三个 worker 的 TLS 析构已发生，关闭调用仍等待 | 通过 |
| 关闭与领取竞态无遗漏或重复 | 单 worker 和 4 worker 各 100 轮，共 51,200 个排队任务；两个线程共同起跑，分别释放 worker 和立即关闭。join 后逐 future/ID 核对：正常结果对应执行一次，broken_promise 对应执行零次，两类数量之和等于任务总数 | 通过 |
| 有运行任务时不提前返回 | 立即关闭已生效后逐个释放运行任务；只要最后一个任务仍阻塞，关闭 future 保持未就绪；全部任务完成后关闭返回，TLS 退出计数等于 worker 数量 | 通过 |

同步使用 latch、future 和退出观察器的条件变量，不用 sleep 推断状态。丢弃 future 的就绪和 broken_promise 是队列丢弃已生效的可观察证据；5 秒超时用于检测挂死，不作为延迟性能指标。领取竞态各轮接受的任务均在关闭前成功提交，按调度结果允许正常执行或丢弃，不要求固定比例。立即关闭分组首次验证的日志记录单 worker 正常执行 9 个、丢弃 25,591 个；4 worker 正常执行 7 个、丢弃 25,593 个，两种结果均被实际触发，未来运行的分布可变化。

[close_race_test.cpp](../tests/close_race_test.cpp) 关闭竞争与自等待进度 **6/6**；每项分别验证 1 个和 4 个 worker，所有关闭通过公共产品接口执行：

| 项目 | 验证范围 | 结果 |
| --- | --- | --- |
| 首次生效关闭方式保持到底 | 分别明确优雅和立即关闭先成为负责人，再启动 8 个混用两入口的调用者；状态和负责人保持不变，100 个排队任务分别全部执行一次或全部丢弃且执行零次 | 通过 |
| 关闭中其他调用者等待，不重复 join | 立即关闭负责人先阻塞在丢弃对象的析构中，再放行析构但保留阻塞 worker；两个阶段 8 个混合调用者均未返回，完成后全体正常返回、worker 退出计数正确、集合为空且负责人清空 | 通过 |
| 本池 worker 自关闭抛 logic_error | 在 RUNNING、SHUTDOWN、SHUTDOWN_NOW 三种状态中，各 worker 调用两个关闭入口均抛 logic_error；运行状态拒绝后池仍可接受和执行任务，外部关闭正常完成 | 通过 |
| 关闭负责人用户析构重入抛 logic_error | 丢弃 Task 持有最后一个捕获对象引用；其析构线程 ID 等于外部关闭负责人、不是本池 worker，重入两个入口分别抛 logic_error，原关闭可继续完成 | 通过 |
| 其他池 worker 不被误判 | 来源池 worker 分别对目标池执行优雅/立即关闭并成为目标池负责人，目标队列结果符合关闭方式；之后来源身份仍正确，自关闭仍拒绝，来源池仍可执行任务 | 通过 |
| 关闭后重复关闭直接返回 | 两种首次关闭各验证完成后的 TERMINATED、空 worker 集合和清空负责人；随后串行混用两入口各 100 次，再由 8 个线程并发混用，均正常返回，退出计数不变且 submit 仍拒绝 | 通过 |

本组使用只读友元观察状态、负责人和 worker 身份，不修改内部状态。worker 集合只在所有关闭调用已完成后读取，避免与队列锁外回收竞争。等待关系用 latch 和 future 建立，状态通知和完成等待均有 5 秒限时；未设置 sleep 或计时性能断言。无重复回收由调用正常返回、TLS 退出计数及完成后的空集合验证，结合源码中 TERMINATED 复查与唯一回收路径核对，不直接注入或计数内部 join。

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

[lifecycle_test.cpp](../tests/lifecycle_test.cpp) 覆盖生命周期与异常安全需求 **6/6**，分组共 **12/12**，全部使用产品实现（无生命周期/stop 定义桩），每个用例由 CTest 独立运行并设置 30 秒超时。首项为数量规则回归，随后 6 项验证以下需求：

| 项目 | 验证范围 | 结果 |
| --- | --- | --- |
| 指定数量与默认数量（回归保留） | 显式 3 个 worker 全部可执行任务；默认构造的 worker 数量等于 `hardware_concurrency()`（返回 0 时保底 1）；两者提交的任务都能取回结果并正常关闭 | 通过 |
| 构造参数 worker_count=0 抛 invalid_argument | 显式 0 抛 std::invalid_argument 且信息含 positive；拒绝后仍可正常构造、执行任务与关闭。不以对象大小或未登记的 TLS 计数推断是否曾创建线程 | 通过 |
| 创建部分 worker 失败时回收已创建线程 | 先测量 1 个和 2 个 worker 的构造分配序列，由差值推出单个 worker 的分配组，并断言组内首次分配等于 sizeof(work_thread)（即 work_thread 对象）；注入点取第 2 个 worker 组的首次分配，因此到达该次数即证明首个 worker 已完整创建。bad_alloc 原样传播，首个 worker 对象内存已释放（线程已 join），随后新建池可正常执行任务 | 通过 |
| 未显式关闭时析构排空队列 | 4 个 worker 全部被阻塞任务占住后提交 12 个任务，析构在独立线程执行；只读观察确认已进入 SHUTDOWN 且 12 个任务仍排队，然后停止观察再放行 worker。析构此前不返回，返回后 12 个 future 全部就绪、结果正确、逐任务只执行一次，TLS 退出计数等于 worker 数量 | 通过 |
| 显式关闭完成后析构不重复回收 | 显式优雅关闭与显式立即关闭后分别析构，退出计数不再增加；完成后串行混用两个入口重复关闭 32 次，状态为 TERMINATED、负责人清空、worker 集合为空、退出计数不变；未显式关闭的池由析构回收一次 | 通过 |
| 入队分配失败时已有队列任务保持不变 | 唯一 worker 被占住，队列中已有 6 个任务；先量出成功入队的最小准备分配数（128 次采样取最小值，容器扩容留在注入关闭后进行），再放行该次数并让紧随其后的队列扩容分配失败。失败后状态仍为 RUNNING、负责人清空、worker 集合不变、队列长度与失败前一致，已有任务 future 仍未就绪且未被调用；关闭前再次提交成功，放行后明确断言原 6 个任务的执行顺序为 0 至 5、各执行一次，所有成功接受的任务均返回正确结果 | 通过 |
| 用户捕获对象销毁在队列锁外进行 | 捕获 unique_ptr 持有地址稳定的探针，submit 移动 callable 不会提前触发对象析构；析构保持进行时另一个线程在 5 秒限时内取得队列锁，不在析构线程自身 try_lock。分别核对 worker 执行后销毁及关闭线程丢弃时销毁的线程 ID 和各一次计数；丢弃捕获销毁时已领取捕获仍存活，被丢弃 callable 未运行 | 通过 |

后 5 项为关闭与重入补充：真实 worker 经两个公共入口及直接 stop 自关闭均抛 logic_error，池仍可执行任务；两种首次方式各以一个负责人和 8 个混合接口调用者验证等待及无升级；优雅队列逐任务执行一次，立即队列不执行且 future 反馈 broken_promise，已领取任务继续完成；完成后 worker 集合为空、负责人 ID 清空且重复关闭正常返回。丢弃捕获析构重入两个关闭接口得到 logic_error，submit 得到拒绝，同一线程关闭另一池正常；局部丢弃队列首次分配 bad_alloc 时状态/负责人未改变，已有任务仍可完成并可重试关闭。

首次补齐时的临时故障核对记录保留在第 7 节历史记录。此前测试核对在独立的 build/ucrt64-debug/lifecycle-review 构建中修改产品源码副本，未改动实际 src/thread_pool.cpp：丢弃捕获持队列锁销毁、已执行捕获持队列锁销毁，均被独立锁观察线程的完成断言捕获；析构改用立即关闭被 SHUTDOWN 状态断言捕获。三个故意错误版本均按预期失败，正常产品完整回归 37/37 通过。此验证构建不加入产品或 CTest。

并发关闭、无升级、负责人重入及显式关闭后析构均已通过真实接口验证。worker 和负责人重入析构的 terminate 路径已接入，尚无子进程测试，不将当前行为测试视为 terminate 路径验证。

## 6. 生命周期验证边界与后续补充

以下行为已由现有测试覆盖，使用 latch、promise/future 与受控任务建立确定顺序，超时用于防止挂死，不用 sleep 推断关闭已生效。尚未验证的路径单独列在本节末尾。

- 优雅关闭：阻塞任务解除后，所有已接受任务恰好执行一次，future 可读，空闲和忙碌 worker 均被 join。
- 立即关闭：先确保部分任务已领取，其余任务排队；关闭生效后排队 callable 不运行，future 报 broken_promise，已领取任务正常完成。
- 并发/重复关闭：多个调用者均等同一次关闭完成，只有首次关闭者 join；混用两种接口不改变首次方式。用受控同步确认首次方式已生效，再启动后续调用。
- worker 自关闭：两种接口均在修改状态和等待前抛 logic_error，未捕获错误经原类型 future 反馈，不妨碍后续任务。
- 负责人重入：让捕获对象在关闭线程清理未执行 Task 时析构，在析构内分别调用两种关闭接口并捕获 logic_error；外层关闭完成、其他调用者获通知，方式与进度不被改变。控制最后一个共享状态引用，避免 future 延长捕获生命周期而绕过目标路径。
- 身份范围：只拒绝同线程正在关闭同一池的重入；其他池不应因身份匹配错误被拒绝。验证完成后负责人 ID 清空，后续顺序关闭正常返回。
- 构造/析构：显式 0、指定数量与默认数量规则、析构优雅等待与已关闭后不重复 join；部分构造失败按运行时测量出的分配序列定位到第 2 个 worker 组的首次分配，验证已创建 worker 的回收，不增加产品接缝或耗尽系统线程资源。
- 锁边界：任务内 submit、锁外捕获析构，以及立即关闭清理时用户析构进入池接口获得拒绝/重入反馈。不能让异常逃逸析构。

尚未覆盖：worker 和负责人重入析构本池的 terminate 子进程测试、操作系统线程创建失败注入，以及 hardware_concurrency() 返回 0 的受控路径。这些逻辑已在源码中实现，当前测试通过不表示上述路径已被实际触发。terminate 测试须隔离在独立进程，不能结束其余测试。

## 7. 性能与验证记录

### 文档与注释整理（2026-10-08）

README 补充最小使用和链接示例；API 只描述调用契约，DESIGN 描述当前同步实现；实现与审核记录区分当前状态和历史阶段。校正性能验证进度、CTest 数量和生命周期提交状态，保留现有测试覆盖限制及原始性能归档。源码注释统一未领取/已领取边界、锁职责、自等待和 future 捕获生命周期说明。

本轮产品源码仅修改注释，去除行注释和空白后与 HEAD 一致。ucrt64-debug 配置、构建（含头文件检查）及 CTest 37/37 通过；README C++ 示例成功编译、链接静态库并运行，7 份文档的 82 个本地链接检查通过，Git 差异格式检查通过。

### 行为测试及性能验证记录（2026-10-08）

核心功能模块 1 至 4 均有产品实现，无生命周期/stop 定义桩；submit 测试仍使用友元状态夹具。基础任务执行 5/5、优雅关闭 6/6、立即关闭 6/6、关闭竞争与自等待 6/6 通过；生命周期与异常安全需求 6/6，相关分组 12/12 通过，前四组每项验证单 worker 和 4 worker。完整回归包含立即关闭领取竞态共 200 轮、51,200 个任务。Debug 和 Release 的完整 CTest 均为 37/37。性能验证阶段增加 Release 性能基准和统计记录，并对测试分配器加 noinline，避免 GCC -O3 内联测试替换 new/delete 后报 mismatched-new-delete；产品和基准优化参数不变。产品源码未修改。

### 已完成的性能验证

扩展轮使用 `run.ps1 -Suite advanced -Rounds 10`，84 组配置、840 次正式测量及 84 次预热全部校验通过，280 项统计包含中位数、范围、分位数、CV。固定总量 6,553,600 次递推，粒度 32 至 65,536 次；2 worker 首次超过单 worker 为 512 次（串行每任务 0.930 μs），4/8 为 1024 次（1.849 μs），完整样本耗时区间均分离。受控 worker 暂停消费时，10 万任务平均提交约 0.44 μs，并发消费约 0.55–0.63 μs。10 万积压任务全部反馈观察约 18 ms，关闭返回后异常 get/释放 future 约 200 ms；所有排队 callable 零执行，所有 future 均 broken_promise，已领取任务正常返回。计时不把观察遍历、门控等待或异常读取解释为纯销毁开销。扩展记录见 [实测报告](../benchmarks/results/20261008-230549/report.md)，方法见 PERFORMANCE.md；Release 完整 CTest 37/37，该性能验证阶段产品源码未修改。

2026-10-08 使用 ucrt64-release、GNU GCC 16.2.0、-O3 -DNDEBUG -std=c++20，在 i7-14650HX（24 个逻辑处理器）、Windows 25H2 build 26200.9457 上完成 42 组配置、各 1 次预热及 10 次正式测量；共 420 次正式记录和 42 次预热，全部计算结果校验通过。CPU 256 个任务、各 200,000 次递推，8 worker 相对串行中位数加速比 6.855x；极短/轻量任务的线程池执行明显慢于串行，多 worker 也低于单 worker 的轻量吞吐，均如实记录。

提交计时和结果收集计时分开保存；吞吐及 CPU 使用完整批次时间。关闭测量分别覆盖两个入口、空闲/半数/全部繁忙，包含剩余固定计算与 join。汇总保存中位数、min/max、四分位数、CV；没有删除离群值。首轮汇总脚本单元素数组展开问题修正后，用原始数据重新汇总，没有重新选择测量。完整方法和命令见 [PERFORMANCE.md](PERFORMANCE.md)，基础组记录见 [实测报告](../benchmarks/results/20261008-224449/report.md)、[samples.csv](../benchmarks/results/20261008-224449/samples.csv)、[summary.csv](../benchmarks/results/20261008-224449/summary.csv) 和 [environment.json](../benchmarks/results/20261008-224449/environment.json)。

### 历史实施记录

以下按实施顺序保留当时状态；其中的“本轮”“待实现”和测试桩描述不代表当前源码，当前状态见第 1、5 节及本节验证记录。

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

随后按用户要求补齐“生命周期与异常安全”分组：把原三项整体式生命周期测试拆成逐项 CTest，最终为 12 项（含保留的指定/默认数量回归）。拆分后的校验同步修正了三处测试自身缺陷：入队分配失败用例原先在 worker 被占住时等待任务执行完而挂死，改为在冻结队列上测量准备分配数并只让随后的队列扩容分配失败；锁外销毁用例原先依赖会被立即关闭一并丢弃的已领取任务，且捕获对象在 submit 准备阶段被移动后析构会提前触发信号，改为“阻塞任务 → 已领取任务 → 丢弃任务”的确定顺序并只武装真正进入队列的捕获；部分创建失败用例原先按对象大小计数，实际失败发生在首个 worker 的线程机制分配上，并未走到回收已创建线程的路径，改为先测量 1/2 个 worker 的构造分配序列、推导单 worker 分配组后再定位第 2 个 worker 组的首次分配。以临时改动产品代码核对四个失败路径均被捕获，随后还原产品代码。ucrt64-debug 配置、构建（含头文件检查）及全部 CTest（37/37）通过，生命周期分组连续 5 轮重复运行通过；改动未提交。
