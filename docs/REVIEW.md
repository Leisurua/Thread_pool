# 前三轮精简 review

[项目入口](../README.md) · [实际实现进度](IMPLEMENTATION.md) · [目标 API](API.md) · [设计](DESIGN.md)

前三轮代码精简与测试适配均已完成，用户已审核确认无大问题。分支为 `refactor/simplify-thread-pool`，精简前基线为 `d0445e9`。第一轮 `b4340f5`、第二轮 `68daae6` 已提交；第三轮与审核后的 hpp 顺序整理一并纳入本次本地提交，随后逐步实现未完成功能。

## 累计变化

| 轮次 | 已删除 | 当前行为 | 主要检查位置 |
| --- | --- | --- | --- |
| 1 | 五种自定义异常、异常包装、cancel、共享取消标志 | 准备/入队失败及用户任务异常保留原类型；拒绝提交抛 runtime_error；未执行 Task 销毁反馈 broken_promise | thread_pool.hpp 的 Task/model 和 submit；两份异常/丢弃测试 |
| 2 | wait/wait_result、Task ID、set、历史丢弃记录、finish_task、专用条件变量和等待资格 | submit 直接入队；worker 锁外执行/销毁后直接进入下一轮；通过各任务 future 等待结果 | thread_pool.hpp 的成员与 submit；thread_pool.cpp；并发提交、乱序完成和分配失败测试 |
| 3 | pause/resume、PAUSED、change_state 和暂停测试桩 | 仅 RUNNING 接受；worker 等待“非 RUNNING 或队列非空”；测试用 latch 控制排队和领取边界 | 状态枚举、接口声明、worker 等待谓词；FIFO、排空、丢弃及空队列测试 |

保留 Task/base/model/packaged_task、move-only 和 std::ref 支持、callable 左值/参数右值调用、值/void/左值引用结果、固定 worker、FIFO、jthread、TLS 身份检测及静态库结构。成功入队仍是接受边界；执行、用户对象销毁及任务丢弃反馈仍在主锁外。

## 建议 review 顺序

1. [thread_pool.hpp](../include/thread_pool.hpp)：Task 仅持有类型擦除对象；submit 锁外准备，锁内检查状态和入队，异常清理先释放锁；未来关闭相关成员仍为待实现依赖。
2. [thread_pool.cpp](../src/thread_pool.cpp)：检查等待和退出谓词、FIFO 领取，以及执行/销毁的锁边界。
3. [submit_test.cpp](../tests/submit_test.cpp)：泛型结果与异常、broken_promise、锁外准备、多生产者恰好执行一次、队列扩容失败后已有任务保持原状并可重试。
4. [worker_test.cpp](../tests/worker_test.cpp)：真实 worker 的嵌套 submit、异常后继续执行、latch 控制 FIFO/领取边界、乱序 future、多 worker 恰好执行一次、锁外析构重入 submit、空队列通知与退出。
5. [API.md](API.md) 和 [IMPLEMENTATION.md](IMPLEMENTATION.md)：区分已经实现的精简行为与仍待实现的生命周期契约。

在本次精简提交上，于项目根目录查看前三轮累计源码/测试差异：

```powershell
git diff d0445e9 HEAD -- include/thread_pool.hpp src/thread_pool.cpp tests/submit_test.cpp tests/worker_test.cpp
```

查看第三轮及审核后调整的提交差异与当前状态：

```powershell
git show HEAD
git status --short --branch
```

审核后仅整理 hpp 顺序：公共接口为生命周期、submit、关闭；私有部分为 Task、worker 身份守卫、辅助方法、TLS 和成员变量；头文件底部先放池实现，再放 worker 生命周期实现。实例成员顺序保持不变，worker 集合仍最后声明；保留用户调整的枚举值与注释。本文随本次提交保存为审核记录。

## 已完成验证与边界

本轮 `ucrt64-debug` 配置、构建（含独立/重复包含头文件检查）、两项 CTest 和 Git 差异格式检查通过；源码和测试中无暂停/快照依赖残留。两项行为测试仍使用池生命周期/状态桩，不代表完整公共线程池已经可用。队列扩容分配失败注入使用本机 UCRT64 标准库实测准备分配数，不硬编码对象大小或依赖产品进度记录。

## review 后的未完成功能

前三轮审核已通过，接下来逐步处理池构造及部分创建失败回收、默认优雅析构、两种阻塞关闭、唯一关闭负责人和并发调用等待、无关闭升级、worker 自关闭及负责人重入检测。届时移除测试生命周期桩，替换 `join_in_progress_`，接入受 mutex 保护的负责人线程 ID；具体契约见 DESIGN.md 第 6 节。完成正确性和生命周期后再进行性能验证。

本次仅完成第三轮提交及 hpp 顺序整理，未开始生命周期实现。
