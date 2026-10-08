#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Task、submit 和 worker 循环已实现；池生命周期、等待及关闭逻辑仍待实现。
enum class status_t : std::int8_t {
    TERMINATED = -1, // 已关闭，所有 worker 已退出
    SHUTDOWN_NOW = 0, // 立即关闭中：丢弃尚未执行的任务，等待正在执行的任务结束
    RUNNING = 1,//正在运行
    PAUSED = 2, // 暂停
    SHUTDOWN = 3 // 优雅关闭中：继续执行排队任务，可升级为 SHUTDOWN_NOW
};

// 关闭生效后拒绝提交：submit 同步抛出，不返回 future。
class submission_rejected : public std::exception {
public:
    const char* what() const noexcept override { return "thread pool submission rejected"; }
};

// 入队前的提交失败；保留原异常，查询操作不强制重抛。
class submission_error : public std::exception {
public:
    explicit submission_error(std::exception_ptr original) noexcept
        : original_(std::move(original)) {}
    const char* what() const noexcept override { return "thread pool submission failed"; }
    std::exception_ptr original_exception() const noexcept { return original_; }

private:
    std::exception_ptr original_;
};

// 已接受但未领取的任务被丢弃：通过 future.get() 反馈。
class task_cancelled : public std::exception {
public:
    const char* what() const noexcept override { return "thread pool task cancelled"; }
};

// 任务函数调用失败；packaged_task 内部存储结果的异常保持原类型。
class task_execution_error : public std::exception {
public:
    explicit task_execution_error(std::exception_ptr original) noexcept
        : original_(std::move(original)) {}
    const char* what() const noexcept override { return "thread pool task execution failed"; }
    std::exception_ptr original_exception() const noexcept { return original_; }

private:
    std::exception_ptr original_;
};

// 仅保存调用错误消息；非法状态、并发 wait 和 worker 自等待由线程池判定。
class thread_pool_call_error : public std::logic_error {
public:
    explicit thread_pool_call_error(const char* message)
        : std::logic_error(message) {}
};

class thread_pool;

// 具体工作线程，与 thread_pool 互为友元。
class work_thread {
    friend class thread_pool;

public:
    explicit work_thread(thread_pool& pool);
    ~work_thread();

    work_thread(const work_thread&) = delete;
    work_thread& operator=(const work_thread&) = delete;
    work_thread(work_thread&&) = delete;
    work_thread& operator=(work_thread&&) = delete;

private:
    void run(); // 线程池的 worker 循环；领取/完成记录持主锁，调用任务在锁外

    thread_pool& pool_; // 非拥有引用，由线程池协调 worker 生命周期
    std::jthread thread_;
};

// 维护 FIFO 任务队列与固定 worker 集合。
class thread_pool {
    friend class work_thread;
    friend struct thread_pool_test_access;

public:
    // 默认采用硬件并发数，无法获取（返回 0）时采用 1；显式传入 0 应抛 invalid_argument。
    explicit thread_pool(std::size_t worker_count = default_worker_count());
    // 默认优雅关闭；已立即关闭时保持该方式，等待所有 worker 退出。
    // 调用方须协调外部访问；本池 worker 中析构本池应 terminate。
    ~thread_pool();

    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;

    void pause();        // 暂停新领取，不等待已领取任务；重复暂停直接返回
    void resume();       // 恢复领取；重复恢复直接返回
    void shutdown();     // 阻塞优雅关闭，暂停不阻止排队任务执行
    void shutdown_now(); // 阻塞立即关闭，丢弃未领取任务，不中断已领取任务
    // 关闭开始后 pause/resume 抛调用错误；并发关闭共用最终退出过程。
    // 本池 worker 禁止调用两个关闭接口，须在状态变更前抛调用错误。

    // 按值保存：左值复制、右值移动；借用须显式 std::ref/std::cref。
    // 保存的 callable 作为左值调用，保存的参数作为右值传入。
    // 支持 move-only callable/参数，以及值、void、左值引用结果。
    // 不支持右值引用结果；由返回类型推导和 static_assert 检查。
    // 成功入队即接受；拒绝/提交失败同步抛出，任务结果经 future 反馈。
    // submit 完美转发保存函数和参数，构造 packaged_task<R()> 并先取得 future。
    // 再将 packaged_task 移入 Task::model，类型擦除后直接入队。
    // Task 不直接保存 promise/future。成功入队后返回 future 句柄，不等待任务执行结束；
    // 调用方通过 future.wait()/get() 等待就绪或获取结果。
    // 准备 Task/结果反馈在锁外进行；锁内统一检查状态、确定 ID 和入队。
    template<class F, class... Args>
    auto submit(F&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>&,
                                           std::decay_t<Args>&&...>>;
    enum class wait_result {
        completed, // 快照全部执行结束（包含异常结束），没有丢弃
        cancelled  // 快照全部处理结束，且至少一个任务被丢弃
    };

    // 等待锁内确定的任务快照，不关闭池；后续提交不扩展范围。
    // 同时只允许一个有效 wait，禁止本池 worker 调用。
    // 暂停/关闭时允许池外调用；暂停可能持续阻塞，历史丢弃计入结果。
    wait_result wait();

private:
    using task_id_type = std::uint64_t;

    // 每个线程记录所属池，供 wait/关闭接口识别本池 worker，由私有守卫维护。
    inline static thread_local const thread_pool* current_worker_pool_ = nullptr;

    // worker 入口登记所属池，离开作用域时恢复；work_thread 通过友元访问。
    class worker_identity_guard {
    public:
        explicit worker_identity_guard(const thread_pool* pool) noexcept
            : previous_(current_worker_pool_) {
            current_worker_pool_ = pool;
        }
        ~worker_identity_guard() noexcept {
            current_worker_pool_ = previous_;
        }

        worker_identity_guard(const worker_identity_guard&) = delete;
        worker_identity_guard& operator=(const worker_identity_guard&) = delete;
        worker_identity_guard(worker_identity_guard&&) = delete;
        worker_identity_guard& operator=(worker_identity_guard&&) = delete;

    private:
        const thread_pool* previous_;
    };

    // 外层为统一类型；内部 model<R> 持有 submit 准备好的 packaged_task<R()>。
    class Task {
        friend class thread_pool; // submit 在正式入队时确定 ID，避免准备顺序替代接受顺序

    public:
        // packaged_task 与 submit 已取得的 future 继续关联同一个共享状态。
        template<class R>
        Task(task_id_type id, std::packaged_task<R()>&& packaged_task,
             std::shared_ptr<bool> cancelled)
            : id_(id),
              task_(std::make_unique<model<R>>(
                  std::move(packaged_task), std::move(cancelled))) {}

        ~Task() = default;

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        Task(Task&&) noexcept = default;
        Task& operator=(Task&&) noexcept = default;

        // 原结果/异常保存在 packaged_task 的共享状态中，统一调用接口返回 void。
        // 池保证执行/取消二选一且只调用一次，不对已移动的 Task 调用。
        void operator()() noexcept { task_->execute(); }
        void cancel() noexcept { task_->cancel(); }

        task_id_type id() const noexcept { return id_; }

    private:
        class base {
        public:
            virtual ~base() = default;
            virtual void execute() noexcept = 0;
            virtual void cancel() noexcept = 0;
        };

        // 模型只接管已构造的 packaged_task，不构造 callable、不获取 future。
        template<class R>
        class model final : public base {
        public:
            model(std::packaged_task<R()>&& packaged_task, std::shared_ptr<bool> cancelled)
                : packaged_task_(std::move(packaged_task)),
                  cancelled_(std::move(cancelled)) {}

            void execute() noexcept override { packaged_task_(); }

            void cancel() noexcept override {
                *cancelled_ = true;
                packaged_task_(); // 只存入 task_cancelled，不调用原函数
            }

        private:
            std::packaged_task<R()> packaged_task_;
            std::shared_ptr<bool> cancelled_;
        };

        task_id_type id_;
        std::unique_ptr<base> task_;
    };

    static std::size_t default_worker_count() noexcept;
    bool is_current_worker() const noexcept;
    void change_state(status_t next); // 状态变更与队列操作共用主互斥锁
    void close(status_t requested);   // 共用关闭流程、升级与单次 join
    void finish_task(task_id_type id, bool cancelled); // 记录进度并通知 wait

    // 状态、队列、快照进度与 wait/join 资格统一由此锁保护。
    // callable、取消反馈、用户对象销毁与 worker join 在锁外。
    std::mutex mutex_;
    std::condition_variable task_ready_;
    std::condition_variable progress_changed_;
    std::condition_variable shutdown_complete_;
    status_t state_;
    std::queue<Task> tasks_;

    // 包含排队与已领取的未处理 ID；有序 set 的最小值用于判断 wait 快照进度。
    std::set<task_id_type> unfinished_tasks_;
    task_id_type last_accepted_id_;
    task_id_type first_cancelled_id_; // 0 表示尚无丢弃，区分快照内/外丢弃
    bool wait_active_;
    bool join_in_progress_;

    // 最后声明，逆序析构 worker 时同步依赖仍然存活。
    std::vector<std::unique_ptr<work_thread>> worker_threads_;
};

inline work_thread::work_thread(thread_pool& pool)
    : pool_(pool), thread_([this] { run(); }) {}

// 池须先设置退出状态并通知 worker；jthread 的 stop 请求不替代池状态协议。
inline work_thread::~work_thread() = default;

inline std::size_t thread_pool::default_worker_count() noexcept {
    const auto count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : count;
}

inline bool thread_pool::is_current_worker() const noexcept {
    return current_worker_pool_ == this;
}

inline void thread_pool::finish_task(task_id_type id, bool cancelled) {
    {
        std::lock_guard lock(mutex_);
        unfinished_tasks_.erase(id);
        if (cancelled && (first_cancelled_id_ == 0 || id < first_cancelled_id_)) {
            first_cancelled_id_ = id;
        }
    }
    progress_changed_.notify_all();
}

template<class F, class... Args>
auto thread_pool::submit(F&& function, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>&, std::decay_t<Args>&&...>> {
    using result_type = std::invoke_result_t<std::decay_t<F>&, std::decay_t<Args>&&...>;
    static_assert(!std::is_rvalue_reference_v<result_type>,
                  "thread_pool does not support rvalue-reference results");

    // 分阶段包装失败，避免把用户对象构造时抛出的 submission_rejected 误判为池拒绝。
    auto [task, future] = [&] {
        try {
            // 取消标志由 packaged_task 的 callable 与模型共享；执行/取消不并发。
            auto cancelled = std::make_shared<bool>(false);
            auto invocation = [saved_function = std::forward<F>(function),
                               saved_arguments = std::tuple<std::decay_t<Args>...>(
                                   std::forward<Args>(args)...),
                               cancelled]() mutable -> result_type {
                if (*cancelled) {
                    throw task_cancelled{};
                }
                try {
                    return std::apply(
                        [&saved_function](auto&&... arguments) -> result_type {
                            return std::invoke(
                                saved_function,
                                std::forward<decltype(arguments)>(arguments)...);
                        },
                        std::move(saved_arguments));
                } catch (...) {
                    throw task_execution_error(std::current_exception());
                }
            };

            std::packaged_task<result_type()> packaged_task(std::move(invocation));
            auto future = packaged_task.get_future();
            Task task(0, std::move(packaged_task), std::move(cancelled));
            return std::pair(std::move(task), std::move(future));
        } catch (...) {
            throw submission_error(std::current_exception());
        }
    }();

    // 锁比 task 后构造，异常退出时先解锁，再销毁用户函数和参数。
    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    try {
        lock.lock();
    } catch (...) {
        throw submission_error(std::current_exception());
    }

    if (state_ != status_t::RUNNING && state_ != status_t::PAUSED) {
        throw submission_rejected{};
    }

    try {
        if (last_accepted_id_ == std::numeric_limits<task_id_type>::max()) {
            throw std::overflow_error("thread pool task ID exhausted");
        }
        const task_id_type id = last_accepted_id_ + 1;
        task.id_ = id;
        unfinished_tasks_.insert(id);
        try {
            tasks_.push(std::move(task));
        } catch (...) {
            unfinished_tasks_.erase(id);
            throw;
        }
        last_accepted_id_ = id;
    } catch (...) {
        throw submission_error(std::current_exception());
    }

    lock.unlock();
    task_ready_.notify_one();
    return future;
}

#endif // THREAD_POOL_HPP
