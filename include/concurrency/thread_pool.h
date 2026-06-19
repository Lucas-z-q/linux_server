#ifndef THREAD_POOL_H_
#define THREAD_POOL_H_

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

class ThreadPool {
   public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    // stop the thread pool and wait for all threads to finish
    void stop();

    // submit a task to the thread pool and get a future for the result
    template <class F, class... Args>
    auto submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>>;

   private:
    // worker function for each thread
    void worker();

    bool Taketask(std::function<void()> &task);

    // flag to indicate if the pool is stopping
    bool stop_flag_;

    // mutex and condition variable for task queue synchronization
    std::mutex queue_mutex_;
    std::condition_variable condition_;

    // task queue
    std::queue<std::function<void()>> tasks_;

    // vector of worker threads
    std::vector<std::thread> workers_;
};

template <class F, class... Args>
auto ThreadPool::submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    auto task =
        std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // don't allow enqueueing after stopping the pool
        if (stop_flag_)
            throw std::runtime_error("submit on stopped ThreadPool");

        tasks_.emplace([task]() { (*task)(); });
    }
    condition_.notify_one();
    return res;
}

#endif  // THREAD_POOL_H_