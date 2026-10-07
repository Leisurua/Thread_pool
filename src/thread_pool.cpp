#include "thread_pool.hpp"

#include <optional>

void work_thread::run() {
    thread_pool::worker_identity_guard identity(&pool_);

    for (;;) {
        std::optional<thread_pool::Task> task;
        {
            std::unique_lock lock(pool_.mutex_);
            pool_.task_ready_.wait(lock, [this] {
                return pool_.state_ == status_t::SHUTDOWN
                    || pool_.state_ == status_t::SHUTDOWN_NOW
                    || pool_.state_ == status_t::TERMINATED
                    || (pool_.state_ == status_t::RUNNING && !pool_.tasks_.empty());
            });

            if (pool_.state_ == status_t::SHUTDOWN_NOW
                || pool_.state_ == status_t::TERMINATED
                || (pool_.state_ == status_t::SHUTDOWN && pool_.tasks_.empty())) {
                return;
            }

            // 领取与状态检查共用主锁；领取后执行不会被暂停或立即关闭撤回。
            task.emplace(std::move(pool_.tasks_.front()));
            pool_.tasks_.pop();
        }

        const auto id = task->id();
        (*task)(); // 用户异常由 packaged_task 保存，worker 继续循环。
        task.reset(); // 用户对象在锁外销毁，完成后再发布处理进度。
        pool_.finish_task(id, false);
    }
}
