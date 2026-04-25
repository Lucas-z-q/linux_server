#include <gtest/gtest.h>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <mutex>
#include "concurrency/thread_pool.h" // 替换为你的真实头文件路径

using namespace std::chrono_literals;

// 1. 测试基础的任务提交和返回值获取
TEST(ThreadPoolTest, BasicSubmitAndReturn)
{
    ThreadPool pool(2); // 启动 2 个工作线程

    // 提交一个返回整数的任务
    auto future = pool.submit([]()
                              { return 42; });

    // 等待并获取结果
    EXPECT_EQ(future.get(), 42);
}

// 2. 测试带有参数的函数提交
TEST(ThreadPoolTest, SubmitWithArguments)
{
    ThreadPool pool(2);

    auto future = pool.submit([](int a, int b)
                              { return a + b; }, 10, 20);

    EXPECT_EQ(future.get(), 30);
}

// 3. 测试并发执行与数据安全 (使用 std::atomic 防止竞态条件)
TEST(ThreadPoolTest, ConcurrentExecution)
{
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    const int task_count = 1000;
    std::vector<std::future<void>> futures;

    // 瞬间塞入 1000 个任务
    for (int i = 0; i < task_count; ++i)
    {
        futures.push_back(pool.submit([&counter]()
                                      {
                                          counter++; // 原子操作，多线程安全
                                      }));
    }

    // 等待所有任务完成
    for (auto &f : futures)
    {
        f.get();
    }

    // 验证是否执行了 1000 次
    EXPECT_EQ(counter.load(), task_count);
}

// 4. 验证是否真的在多个线程中并行执行
TEST(ThreadPoolTest, RealParallelism)
{
    const int thread_count = 4;
    ThreadPool pool(thread_count);

    std::mutex mtx;
    std::unordered_set<std::thread::id> thread_ids;
    std::vector<std::future<void>> futures;

    // 提交一批阻塞性质的任务，迫使线程池必须唤醒所有线程来处理
    for (int i = 0; i < thread_count * 2; ++i)
    {
        futures.push_back(pool.submit([&mtx, &thread_ids]()
                                      {
            std::this_thread::sleep_for(50ms); // 模拟耗时任务，防止一个线程瞬间跑完所有任务
            std::lock_guard<std::mutex> lock(mtx);
            thread_ids.insert(std::this_thread::get_id()); }));
    }

    for (auto &f : futures)
    {
        f.get();
    }

    // 验证执行任务的独立线程数量是否等于线程池设置的数量
    EXPECT_EQ(thread_ids.size(), thread_count);
}

// 5. 测试停机逻辑 (Stop 或析构)
TEST(ThreadPoolTest, StopRejectsNewTasks)
{
    ThreadPool pool(2);

    // 假设你的线程池暴露了显式的 stop() 方法
    pool.stop();

    // 停止后提交新任务应该抛出异常 (根据你之前提供的 submit 代码)
    EXPECT_THROW({ pool.submit([]
                               { return 1; }); }, std::runtime_error);
}

// 6. 测试停机时旧任务能够执行完毕
TEST(ThreadPoolTest, FinishPendingTasksOnStop)
{
    auto pool = std::make_unique<ThreadPool>(2);
    std::atomic<int> counter{0};

    // 提交两个需要一点时间才能完成的任务
    pool->submit([&counter]()
                 {
        std::this_thread::sleep_for(100ms);
        counter++; });
    pool->submit([&counter]()
                 {
        std::this_thread::sleep_for(100ms);
        counter++; });

    // 立即销毁线程池（调用析构函数）
    // 析构函数应该会阻塞等待（join）这两个任务执行完毕
    pool.reset();

    // 验证线程池销毁后，任务是否安全执行完了
    EXPECT_EQ(counter.load(), 2);
}