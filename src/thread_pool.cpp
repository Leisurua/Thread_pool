#include "thread_pool.hpp"

#include <exception>
#include <optional>

thread_pool::thread_pool(std::size_t worker_count)
    : state_(status_t::RUNNING) {
    if (worker_count == 0) {
        throw std::invalid_argument("thread pool worker count must be positive");
    }

    // 默认实参已由 default_worker_count() 提供，不在此重新计算数量。
    // 先预留容量，避免启动 worker 后因 vector 扩容销毁临时 worker 而等待不退出的线程。
    worker_threads_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            worker_threads_.push_back(std::make_unique<work_thread>(*this));
        }
    } catch (...) {
        {
            std::lock_guard lock(queue_mutex_);
            state_ = status_t::SHUTDOWN_NOW;
        }
        task_ready_.notify_all();
        worker_threads_.clear(); // 锁外 join 已创建 worker，再传播原异常。
        throw;
    }
}

thread_pool::~thread_pool() {
    if (is_current_worker()) {
        std::terminate();
    }
    // 共用优雅关闭；负责人重入抛出的异常使 noexcept 析构 terminate。
    stop(status_t::SHUTDOWN);
}

void thread_pool::shutdown() {
    if (is_current_worker()) {
        throw std::logic_error("thread pool worker cannot shut down its own pool");
    }
    {
        std::lock_guard lock(queue_mutex_);
        if (state_ == status_t::TERMINATED) {
            return;
        }
    }
    stop(status_t::SHUTDOWN); // 锁外交给共用流程；入口不改变关闭状态。
}

void thread_pool::shutdown_now() {
    if (is_current_worker()) {
        throw std::logic_error("thread pool worker cannot shut down its own pool");
    }
    {
        std::lock_guard lock(queue_mutex_);
        if (state_ == status_t::TERMINATED) {
            return;
        }
    }
    stop(status_t::SHUTDOWN_NOW);
}

void thread_pool::stop(status_t requested) {
    if (is_current_worker()) {
        throw std::logic_error("thread pool worker cannot shut down its own pool");
    }

    {
        std::lock_guard queue_lock(queue_mutex_);
        if (state_ == status_t::TERMINATED) {
            return;
        }
        if (closing_thread_id_ == std::this_thread::get_id()) {
            throw std::logic_error("thread pool shutdown owner cannot reenter shutdown");
        }
    }

    // 此锁只让并发关闭调用依次进入下面的流程，状态/队列仍由队列锁保护。
    // 预检查已释放队列锁；等待关闭锁时不影响提交和 worker 取任务。
    std::lock_guard shutdown_lock(shutdown_mutex_);
    std::optional<std::queue<Task>> discarded;
    {
        std::lock_guard queue_lock(queue_mutex_);
        // 等到关闭锁后必须复查：前一个关闭调用可能已经完成。
        if (state_ == status_t::TERMINATED) {
            return;
        }
        if (requested == status_t::SHUTDOWN_NOW) {
            discarded.emplace(); // 分配失败时尚未修改状态，两锁均由 RAII 释放。
        }
        state_ = requested;
        closing_thread_id_ = std::this_thread::get_id();
        if (discarded) {
            discarded->swap(tasks_); // 队列锁内确定未领取任务的丢弃边界。
        }
    }
    task_ready_.notify_all();

    discarded.reset(); // 不持队列锁，仍持关闭锁；负责人重入须在取关闭锁前拒绝。
    worker_threads_.clear(); // 当前关闭调用独占流程；不持队列锁，worker 可继续退出。

    {
        std::lock_guard queue_lock(queue_mutex_);
        state_ = status_t::TERMINATED;
        closing_thread_id_ = {};
    }
    // 释放关闭锁后，排队的关闭调用者取得锁并复查 TERMINATED，不重复回收。
}

void thread_pool::enqueue(Task task) {
    // Task 参数先于局部锁构造；失败退出时先解锁，再释放持有用户对象的 Task。
    {
        std::lock_guard lock(queue_mutex_);
        if (state_ != status_t::RUNNING) {
            throw std::runtime_error("thread pool submission rejected");
        }
        if (!task.valid()) {
            throw std::invalid_argument("thread pool cannot enqueue an empty task");
        }
        // Task 为 noexcept 移动；push 成功即接受，分配失败直接传播且已有任务不变。
        tasks_.push(std::move(task));
    }
    task_ready_.notify_one();
}

void work_thread::run() {
    thread_pool::worker_identity_guard identity(&pool_);

    for (;;) {
        std::optional<thread_pool::Task> task;
        {
            std::unique_lock lock(pool_.queue_mutex_);
            pool_.task_ready_.wait(lock, [this] {
                return pool_.state_ != status_t::RUNNING || !pool_.tasks_.empty();
            });

            if (pool_.state_ == status_t::SHUTDOWN_NOW
                || pool_.state_ == status_t::TERMINATED
                || (pool_.state_ == status_t::SHUTDOWN && pool_.tasks_.empty())) {
                return;
            }

            // 领取与状态检查共用队列锁；领取后执行不会被立即关闭撤回。
            task.emplace(std::move(pool_.tasks_.front()));
            pool_.tasks_.pop();
        }

        (*task)(); // 用户异常由 packaged_task 保存，worker 继续循环。
        task.reset(); // 锁外释放 Task；future 仍可能延长用户捕获的生命周期。
    }
}
