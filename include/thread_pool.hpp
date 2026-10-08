#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// 固定 worker、泛型提交、优雅关闭及立即关闭均已实现。
enum class status_t : std::int8_t {
    TERMINATED = -1, // 已关闭，所有 worker 已退出
    SHUTDOWN_NOW = 0, // 立即关闭中：丢弃尚未执行的任务，等待正在执行的任务结束
    RUNNING = 1, // 正在运行
    SHUTDOWN = 2 // 优雅关闭中：继续执行排队任务
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
    void run(); // 线程池的 worker 循环；领取持队列锁，执行和销毁任务在锁外

    thread_pool& pool_;//池
    std::jthread thread_;//工作线程内部实际jthread
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

    // 按值保存：左值复制、右值移动；借用须显式 std::ref/std::cref。
    // 保存的 callable 作为左值调用，保存的参数作为右值传入。
    // 支持 move-only callable/参数，以及值、void、左值引用结果。
    // 不支持右值引用结果；由返回类型推导和 static_assert 检查。
    // 成功入队即接受；拒绝/提交失败同步抛出，任务结果经 future 反馈。
    // submit 完美转发保存函数和参数，构造 packaged_task<R()> 并先取得 future。
    // 再将 packaged_task 移入 Task::model，类型擦除后交给私有 enqueue。
    // Task 不直接保存 promise/future。成功入队后返回 future 句柄，不等待任务执行结束；
    // 调用方通过 future.wait()/get() 等待就绪或获取结果。
    // submit 锁外准备 Task/结果反馈；enqueue 负责状态检查、安全入队和通知。
    template<class F, class... Args>
    auto submit(F&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>&,
                                           std::decay_t<Args>&&...>>;

    void shutdown();     // 阻塞优雅关闭，排空已接受任务
    void shutdown_now(); // 阻塞立即关闭，丢弃未领取任务，不中断已领取任务
    // 并发关闭共用最终退出过程，首次生效的关闭方式保持到底。
    // 本池 worker 禁止调用两个关闭接口，须在状态变更前抛 std::logic_error。

private:
    // 外层为统一类型；内部 model<R> 持有 submit 准备好的 packaged_task<R()>。
    class Task {
    public:
        // packaged_task 与 submit 已取得的 future 继续关联同一个共享状态。
        template<class R>
        explicit Task(std::packaged_task<R()>&& packaged_task)
            : task_(std::make_unique<model<R>>(std::move(packaged_task))) {}

        ~Task() = default;

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        Task(Task&&) noexcept = default;
        Task& operator=(Task&&) noexcept = default;

        // 原结果/异常保存在 packaged_task 的共享状态中，统一调用接口返回 void。
        // 池保证仅执行一次或直接销毁，不对已移动的 Task 调用。
        // 未执行 packaged_task 的销毁通过 future 反馈 broken_promise。
        void operator()() noexcept { task_->execute(); }
        bool valid() const noexcept { return task_ != nullptr; }

    private:
        class base {
        public:
            virtual ~base() = default;
            virtual void execute() noexcept = 0;
        };

        // 模型只接管已构造的 packaged_task，不构造 callable、不获取 future。
        template<class R>
        class model final : public base {
        public:
            explicit model(std::packaged_task<R()>&& packaged_task)
                : packaged_task_(std::move(packaged_task)) {}

            void execute() noexcept override { packaged_task_(); }

        private:
            std::packaged_task<R()> packaged_task_;
        };

        std::unique_ptr<base> task_;
    };//自实现task类进行擦除

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

    static std::size_t default_worker_count() noexcept;
    bool is_current_worker() const noexcept;
    void enqueue(Task task); // 内部接管有效任务，持锁检查/入队，解锁后通知 worker。
    void stop(status_t requested); // 队列锁维护原有任务/状态顺序，关闭锁只互斥并发关闭调用。

    // 每个线程记录所属池，供关闭接口识别本池 worker，由私有守卫维护。（结构化 不暴露对外API）
    inline static thread_local const thread_pool* current_worker_pool_ = nullptr;

    // 只互斥并发关闭调用，持有至 stop 完成；submit 和 worker 不使用此锁。
    // 同时持两锁时固定按 shutdown_mutex_ -> queue_mutex_ 获取。
    std::mutex shutdown_mutex_;
    // 状态、任务队列与负责人身份由队列锁保护；执行、销毁及 join 不持此锁。
    std::mutex queue_mutex_;
    std::condition_variable task_ready_;
    status_t state_;
    std::thread::id closing_thread_id_{}; // 队列锁保护，在获取关闭锁前识别自身重入。
    std::queue<Task> tasks_;

    // 最后声明，逆序析构 worker 时同步依赖仍然存活。
    std::vector<std::unique_ptr<work_thread>> worker_threads_;
};

inline std::size_t thread_pool::default_worker_count() noexcept {
    const auto count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : count;
}

inline bool thread_pool::is_current_worker() const noexcept {
    return current_worker_pool_ == this;
}

template<class F, class... Args>
auto thread_pool::submit(F&& function, Args&&... args)
    -> std::future<std::invoke_result_t<std::decay_t<F>&, std::decay_t<Args>&&...>> {
    using result_type = std::invoke_result_t<std::decay_t<F>&, std::decay_t<Args>&&...>;
    static_assert(!std::is_rvalue_reference_v<result_type>,
                  "thread_pool does not support rvalue-reference results");

    // 锁外准备；准备失败直接传播，用户异常由 packaged_task 存入 future。
    auto invocation = [saved_function = std::forward<F>(function),
                       saved_arguments = std::tuple<std::decay_t<Args>...>(
                           std::forward<Args>(args)...)]() mutable -> result_type {
        return std::apply(
            [&saved_function](auto&&... arguments) -> result_type {
                return std::invoke(
                    saved_function, std::forward<decltype(arguments)>(arguments)...);
            },
            std::move(saved_arguments));
    };
    std::packaged_task<result_type()> packaged_task(std::move(invocation));
    auto future = packaged_task.get_future();
    Task task(std::move(packaged_task));

    enqueue(std::move(task));
    return future;
}

inline work_thread::work_thread(thread_pool& pool)
    : pool_(pool), thread_([this] { run(); }) {}

// 池须先设置退出状态并通知 worker；jthread 的 stop 请求不替代池状态协议。
inline work_thread::~work_thread() = default;

#endif // THREAD_POOL_HPP
