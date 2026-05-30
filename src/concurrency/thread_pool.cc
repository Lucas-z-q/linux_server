#include "concurrency/thread_pool.h"

ThreadPool::ThreadPool(size_t num_thread) : stop_flag_(false) {
    for (size_t i = 0; i < num_thread; ++i) {
        workers_.emplace_back(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::worker() {
    while (true) {
        std::function<void()> task;
        if (!Taketask(task))
            return;
        task();
    }
}

bool ThreadPool::Taketask(std::function<void()> &task) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    condition_.wait(lock, [this] { return stop_flag_ || !tasks_.empty(); });
    if (stop_flag_ && tasks_.empty())
        return false;
    task = std::move(tasks_.front());
    tasks_.pop();
    return true;
}

void ThreadPool::stop() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_flag_ = true;
    }
    condition_.notify_all();
    for (std::thread &worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
}