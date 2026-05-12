#include "concurrency/thread_pool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <atomic>
#include <vector>

TEST(ThreadPoolTest, BasicExecution) {
    ThreadPool pool(2);
    auto fut = pool.submit([] { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

TEST(ThreadPoolTest, ArgumentBinding) {
    ThreadPool pool(2);
    auto fut = pool.submit([](int a, int b) { return a + b; }, 10, 32);
    EXPECT_EQ(fut.get(), 42);
}

TEST(ThreadPoolTest, ConcurrentExecution) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;

    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool.submit([&counter] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            counter++;
        }));
    }

    for (auto& fut : futures) {
        fut.get();
    }

    EXPECT_EQ(counter.load(), 100);
}

TEST(ThreadPoolTest, RejectSubmitAfterStop) {
    ThreadPool pool(2);
    pool.stop();

    EXPECT_THROW({
        pool.submit([] { return 1; });
    }, std::runtime_error);
}

TEST(ThreadPoolTest, MultipleStopCallsAreSafe) {
    ThreadPool pool(2);

    // 提交一个基本任务
    pool.submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });

    // 第一次正常停机
    pool.stop();

    // 第 2~5 次重复停机，验证不抛出异常、不死锁且不引发系统崩溃
    EXPECT_NO_THROW({
        pool.stop();
        pool.stop();
        pool.stop();
        pool.stop();
    });
}

TEST(ThreadPoolTest, StopWaitsForPendingTasks) {
    ThreadPool pool(2);
    std::atomic<int> completed_tasks{0};

    for (int i = 0; i < 5; ++i) {
        pool.submit([&completed_tasks] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            completed_tasks++;
        });
    }

    // stop 会阻塞直到所有入队的任务执行完成
    pool.stop();

    EXPECT_EQ(completed_tasks.load(), 5);
}
