# 前三轮精简 review

[项目入口](../README.md) · [实现与验证](IMPLEMENTATION.md) · [API 契约](API.md) · [设计](DESIGN.md)

本文保留前三轮审核时的范围和验证结果，不作为当前进度说明。前三轮代码精简与测试适配均已完成，用户已审核确认无大问题。分支为 `refactor/simplify-thread-pool`，精简前基线为 `d0445e9`；三轮提交分别为 `b4340f5`、`68daae6`、`722e5fe`，第三轮包含审核后的 hpp 顺序整理。当前实现与验证见 [IMPLEMENTATION.md](IMPLEMENTATION.md)。

## 累计变化

| 轮次 | 已删除 | 精简后的行为 | 主要检查位置 |
| --- | --- | --- | --- |
| 1 | 五种自定义异常、异常包装、cancel、共享取消标志 | 准备/入队失败及用户任务异常保留原类型；拒绝提交抛 runtime_error；未执行 Task 销毁反馈 broken_promise | thread_pool.hpp 的 Task/model 和 submit；两份异常/丢弃测试 |
| 2 | wait/wait_result、Task ID、set、历史丢弃记录、finish_task、专用条件变量和等待资格 | submit 直接入队；worker 锁外执行/销毁后直接进入下一轮；通过各任务 future 等待结果 | thread_pool.hpp 的成员与 submit；thread_pool.cpp；并发提交、乱序完成和分配失败测试 |
| 3 | pause/resume、PAUSED、change_state 和暂停测试桩 | 仅 RUNNING 接受；worker 等待“非 RUNNING 或队列非空”；测试用 latch 控制排队和领取边界 | 状态枚举、接口声明、worker 等待谓词；FIFO、排空、丢弃及空队列测试 |

保留 Task/base/model/packaged_task、move-only 和 std::ref 支持、callable 左值/参数右值调用、值/void/左值引用结果、固定 worker、FIFO、jthread、TLS 身份检测及静态库结构。成功入队仍是接受边界；执行、用户对象销毁及任务丢弃反馈仍在主锁外。

## 前三轮当时的 review 顺序（历史记录）

本节保留 722e5fe 时的检查范围；下述“待实现”描述为当时状态，当前生命周期和关闭已实现，见 IMPLEMENTATION.md。

1. [thread_pool.hpp](../include/thread_pool.hpp)：Task 仅持有类型擦除对象；submit 锁外准备，锁内检查状态和入队，异常清理先释放锁；未来关闭相关成员仍为待实现依赖。
2. [thread_pool.cpp](../src/thread_pool.cpp)：检查等待和退出谓词、FIFO 领取，以及执行/销毁的锁边界。
3. [submit_test.cpp](../tests/submit_test.cpp)：泛型结果与异常、broken_promise、锁外准备、多生产者恰好执行一次、队列扩容失败后已有任务保持原状并可重试。
4. [worker_test.cpp](../tests/worker_test.cpp)：真实 worker 的嵌套 submit、异常后继续执行、latch 控制 FIFO/领取边界、乱序 future、多 worker 恰好执行一次、锁外析构重入 submit、空队列通知与退出。
5. [API.md](API.md) 和 [IMPLEMENTATION.md](IMPLEMENTATION.md)：区分已经实现的精简行为与仍待实现的生命周期契约。

于项目根目录查看前三轮累计源码/测试差异，固定历史终点以排除后续改动：

```powershell
git diff d0445e9 722e5fe -- include/thread_pool.hpp src/thread_pool.cpp tests/submit_test.cpp tests/worker_test.cpp
```

查看第三轮及审核后调整的提交差异：

```powershell
git show 722e5fe
```

审核后仅整理 hpp 顺序：公共接口为生命周期、submit、关闭；私有部分为 Task、worker 身份守卫、辅助方法、TLS 和成员变量；头文件底部先放池实现，再放 worker 生命周期实现。实例成员顺序保持不变，worker 集合仍最后声明；保留用户调整的枚举值与注释。本文随本次提交保存为审核记录。

## 已完成验证与边界

前三轮 `ucrt64-debug` 配置、构建（含独立/重复包含头文件检查）、两项 CTest 和 Git 差异格式检查通过；当时行为测试使用池生命周期/状态桩，不代表完整公共线程池可用。队列扩容分配失败注入使用本机 UCRT64 标准库实测准备分配数，不硬编码对象大小或依赖产品进度记录。后续已实现构造/优雅析构并删除生命周期定义桩，当前进度以 IMPLEMENTATION.md 为准。

## 后续记录入口

生命周期、关闭及 enqueue 拆分已保存为 `3b24ed7`。其后的 37 项行为测试和本机性能记录见 [IMPLEMENTATION.md](IMPLEMENTATION.md) 与 [PERFORMANCE.md](PERFORMANCE.md)。本文仅记录前三轮已确认的用户审核，不新增后续审核结论。
