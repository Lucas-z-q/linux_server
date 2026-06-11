#include "common/logger.h"

#include <gtest/gtest.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace chat {
namespace {

class MemorySink : public ILogSink {
   public:
    bool Write(std::string_view line) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.emplace_back(line);
        return true;
    }

    void Flush() noexcept override { flush_count_.fetch_add(1); }

    std::vector<std::string> Lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    int FlushCount() const { return flush_count_.load(); }

   private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::atomic<int> flush_count_{0};
};

class BlockingSink : public ILogSink {
   public:
    bool Write(std::string_view line) noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.emplace_back(line);
            entered_ = true;
        }
        entered_cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        release_cv_.wait(lock, [this]() { return released_; });
        return true;
    }

    void Flush() noexcept override {}

    bool WaitUntilEntered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return entered_cv_.wait_for(lock, timeout, [this]() { return entered_; });
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        release_cv_.notify_all();
    }

    std::vector<std::string> Lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

   private:
    mutable std::mutex mutex_;
    std::condition_variable entered_cv_;
    std::condition_variable release_cv_;
    std::vector<std::string> lines_;
    bool entered_ = false;
    bool released_ = false;
};

class TempDirectory {
   public:
    TempDirectory() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("linux_server_logger_test_" + std::to_string(getpid()) + "_" + std::to_string(unique));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

   private:
    std::filesystem::path path_;
};

LoggerOptions SyncOptions() {
    LoggerOptions options;
    options.min_level = LogLevel::kDebug;
    options.console = false;
    options.async = false;
    return options;
}

std::vector<std::unique_ptr<ILogSink>> MakeMemorySinks(MemorySink** sink) {
    auto owned_sink = std::make_unique<MemorySink>();
    *sink = owned_sink.get();
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::move(owned_sink));
    return sinks;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

TEST(LoggerTest, WritesAllFourLevelsAndFiltersBelowMinimum) {
    Logger logger;
    MemorySink* sink = nullptr;
    ASSERT_TRUE(logger.Initialize(SyncOptions(), MakeMemorySinks(&sink)));

    logger.Write(LogLevel::kDebug, "test", "debug");
    logger.Write(LogLevel::kInfo, "test", "info");
    logger.Write(LogLevel::kWarn, "test", "warn");
    logger.Write(LogLevel::kError, "test", "error");
    ASSERT_EQ(sink->Lines().size(), 4u);

    LoggerOptions options = SyncOptions();
    options.min_level = LogLevel::kWarn;
    ASSERT_TRUE(logger.Initialize(options, MakeMemorySinks(&sink)));
    logger.Write(LogLevel::kDebug, "test", "hidden debug");
    logger.Write(LogLevel::kInfo, "test", "hidden info");
    logger.Write(LogLevel::kWarn, "test", "visible warn");
    logger.Write(LogLevel::kError, "test", "visible error");

    const auto lines = sink->Lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find("[WARN]"), std::string::npos);
    EXPECT_NE(lines[1].find("[ERROR]"), std::string::npos);
}

TEST(LoggerTest, FormatsTimestampThreadIdStreamAndEscapedNewlines) {
    Logger logger;
    MemorySink* sink = nullptr;
    ASSERT_TRUE(logger.Initialize(SyncOptions(), MakeMemorySinks(&sink)));

    {
        LogMessage message(logger, LogLevel::kInfo, "Net\nModule");
        message.stream() << "value=" << 42 << "\r\nnext";
    }

    const auto lines = sink->Lines();
    ASSERT_EQ(lines.size(), 1u);
    const std::regex format(
        R"(^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z\] \[tid=([0-9]+)\] \[INFO\] \[Net\\nModule\] value=42\\r\\nnext$)");
    std::smatch match;
    ASSERT_TRUE(std::regex_match(lines[0], match, format)) << lines[0];
    EXPECT_EQ(std::stoll(match[1].str()), static_cast<std::int64_t>(syscall(SYS_gettid)));
    EXPECT_EQ(std::count(lines[0].begin(), lines[0].end(), '\n'), 0);
    EXPECT_EQ(std::count(lines[0].begin(), lines[0].end(), '\r'), 0);
}

TEST(LoggerTest, MacroDoesNotEvaluateFilteredStreamExpression) {
    Logger& logger = Logger::Instance();
    LoggerOptions options = SyncOptions();
    options.min_level = LogLevel::kError;
    MemorySink* sink = nullptr;
    ASSERT_TRUE(logger.Initialize(options, MakeMemorySinks(&sink)));

    int evaluated = 0;
    LOG_DEBUG("macro") << ++evaluated;
    LOG_ERROR("macro") << "visible";

    EXPECT_EQ(evaluated, 0);
    ASSERT_EQ(sink->Lines().size(), 1u);
    logger.Shutdown();
}

TEST(LoggerTest, ConcurrentWritesProduceCompleteDistinctLines) {
    Logger logger;
    LoggerOptions options = SyncOptions();
    options.async = true;
    options.queue_capacity = 4096;
    MemorySink* sink = nullptr;
    ASSERT_TRUE(logger.Initialize(options, MakeMemorySinks(&sink)));

    constexpr int kThreads = 8;
    constexpr int kMessagesPerThread = 100;
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&logger, thread_index]() {
            for (int message_index = 0; message_index < kMessagesPerThread; ++message_index) {
                logger.Write(LogLevel::kInfo, "concurrent",
                             "thread=" + std::to_string(thread_index) + " message=" + std::to_string(message_index));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    logger.Shutdown();

    const auto lines = sink->Lines();
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(kThreads * kMessagesPerThread));
    for (const auto& line : lines) {
        EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 0);
        EXPECT_EQ(line.find("[concurrent]"), line.rfind("[concurrent]"));
    }
}

TEST(LoggerTest, CreatesDirectoriesAndRollsOnlyWhenNextLineExceedsBoundary) {
    TempDirectory temp;
    const auto log_path = temp.path() / "nested" / "server.log";

    Logger sizing_logger;
    MemorySink* sizing_sink = nullptr;
    ASSERT_TRUE(sizing_logger.Initialize(SyncOptions(), MakeMemorySinks(&sizing_sink)));
    sizing_logger.Write(LogLevel::kInfo, "roll", "same");
    const std::size_t line_size = sizing_sink->Lines().front().size() + 1;

    LoggerOptions options = SyncOptions();
    options.file_path = log_path.string();
    options.max_file_size_bytes = line_size * 2;
    options.max_files = 3;
    Logger logger;
    ASSERT_TRUE(logger.Initialize(options));
    logger.Write(LogLevel::kInfo, "roll", "same");
    logger.Write(LogLevel::kInfo, "roll", "same");
    logger.Flush();

    EXPECT_TRUE(std::filesystem::exists(log_path));
    EXPECT_FALSE(std::filesystem::exists(log_path.string() + ".1"));
    EXPECT_EQ(std::filesystem::file_size(log_path), line_size * 2);

    logger.Write(LogLevel::kInfo, "roll", "same");
    logger.Shutdown();
    EXPECT_TRUE(std::filesystem::exists(log_path.string() + ".1"));
    EXPECT_EQ(ReadLines(log_path).size(), 1u);
    EXPECT_EQ(ReadLines(log_path.string() + ".1").size(), 2u);
}

TEST(LoggerTest, KeepsAtMostConfiguredFiles) {
    TempDirectory temp;
    const auto log_path = temp.path() / "server.log";

    Logger sizing_logger;
    MemorySink* sizing_sink = nullptr;
    ASSERT_TRUE(sizing_logger.Initialize(SyncOptions(), MakeMemorySinks(&sizing_sink)));
    sizing_logger.Write(LogLevel::kInfo, "roll", "entry");
    const std::size_t line_size = sizing_sink->Lines().front().size() + 1;

    LoggerOptions options = SyncOptions();
    options.file_path = log_path.string();
    options.max_file_size_bytes = line_size;
    options.max_files = 3;
    Logger logger;
    ASSERT_TRUE(logger.Initialize(options));
    for (int index = 0; index < 5; ++index) {
        logger.Write(LogLevel::kInfo, "roll", "entry");
    }
    logger.Shutdown();

    EXPECT_TRUE(std::filesystem::exists(log_path));
    EXPECT_TRUE(std::filesystem::exists(log_path.string() + ".1"));
    EXPECT_TRUE(std::filesystem::exists(log_path.string() + ".2"));
    EXPECT_FALSE(std::filesystem::exists(log_path.string() + ".3"));
}

TEST(LoggerTest, MaxFilesOneTruncatesCurrentFileOnRoll) {
    TempDirectory temp;
    const auto log_path = temp.path() / "server.log";

    Logger sizing_logger;
    MemorySink* sizing_sink = nullptr;
    ASSERT_TRUE(sizing_logger.Initialize(SyncOptions(), MakeMemorySinks(&sizing_sink)));
    sizing_logger.Write(LogLevel::kInfo, "roll", "first");
    const std::size_t line_size = sizing_sink->Lines().front().size() + 1;

    LoggerOptions options = SyncOptions();
    options.file_path = log_path.string();
    options.max_file_size_bytes = line_size;
    options.max_files = 1;
    Logger logger;
    ASSERT_TRUE(logger.Initialize(options));
    logger.Write(LogLevel::kInfo, "roll", "first");
    logger.Write(LogLevel::kInfo, "roll", "second");
    logger.Shutdown();

    const auto lines = ReadLines(log_path);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("second"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(log_path.string() + ".1"));
}

TEST(LoggerTest, OversizedSingleLineIsWrittenToEmptyFile) {
    TempDirectory temp;
    const auto log_path = temp.path() / "server.log";

    LoggerOptions options = SyncOptions();
    options.file_path = log_path.string();
    options.max_file_size_bytes = 16;
    options.max_files = 2;
    Logger logger;
    ASSERT_TRUE(logger.Initialize(options));
    logger.Write(LogLevel::kInfo, "roll", std::string(128, 'x'));
    logger.Shutdown();

    EXPECT_GT(std::filesystem::file_size(log_path), options.max_file_size_bytes);
    EXPECT_FALSE(std::filesystem::exists(log_path.string() + ".1"));
}

TEST(LoggerTest, FileInitializationFailureIsReported) {
    TempDirectory temp;
    LoggerOptions options = SyncOptions();
    options.file_path = temp.path().string();

    Logger logger;
    EXPECT_FALSE(logger.Initialize(options));
}

TEST(LoggerTest, FlushMakesAsyncWritesVisibleAndShutdownDrainsQueue) {
    Logger logger;
    LoggerOptions options = SyncOptions();
    options.async = true;
    MemorySink* sink = nullptr;
    ASSERT_TRUE(logger.Initialize(options, MakeMemorySinks(&sink)));

    logger.Write(LogLevel::kInfo, "async", "before flush");
    logger.Flush();
    EXPECT_EQ(sink->Lines().size(), 1u);
    EXPECT_GT(sink->FlushCount(), 0);

    for (int index = 0; index < 100; ++index) {
        logger.Write(LogLevel::kInfo, "async", "drain " + std::to_string(index));
    }
    logger.Shutdown();
    EXPECT_EQ(sink->Lines().size(), 101u);
    EXPECT_NO_THROW(logger.Shutdown());
}

TEST(LoggerTest, AsyncSubmissionDoesNotWaitForBlockedSink) {
    Logger logger;
    LoggerOptions options = SyncOptions();
    options.async = true;
    options.queue_capacity = 8;
    auto owned_sink = std::make_unique<BlockingSink>();
    BlockingSink* sink = owned_sink.get();
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::move(owned_sink));
    ASSERT_TRUE(logger.Initialize(options, std::move(sinks)));

    logger.Write(LogLevel::kInfo, "async", "block writer");
    ASSERT_TRUE(sink->WaitUntilEntered(std::chrono::seconds(1)));

    auto submit =
        std::async(std::launch::async, [&logger]() { logger.Write(LogLevel::kInfo, "async", "business thread"); });
    EXPECT_EQ(submit.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);

    sink->Release();
    logger.Shutdown();
    EXPECT_EQ(sink->Lines().size(), 2u);
}

TEST(LoggerTest, FullQueueEvictsOldLowPriorityAndReportsDroppedCount) {
    Logger logger;
    LoggerOptions options = SyncOptions();
    options.async = true;
    options.queue_capacity = 2;
    auto owned_sink = std::make_unique<BlockingSink>();
    BlockingSink* sink = owned_sink.get();
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::move(owned_sink));
    ASSERT_TRUE(logger.Initialize(options, std::move(sinks)));

    logger.Write(LogLevel::kInfo, "queue", "writer blocked");
    ASSERT_TRUE(sink->WaitUntilEntered(std::chrono::seconds(1)));
    logger.Write(LogLevel::kDebug, "queue", "old low priority");
    logger.Write(LogLevel::kInfo, "queue", "new low priority");
    logger.Write(LogLevel::kError, "queue", "important");

    sink->Release();
    logger.Shutdown();

    const auto lines = sink->Lines();
    ASSERT_EQ(lines.size(), 4u);
    const std::string joined = lines[0] + lines[1] + lines[2] + lines[3];
    EXPECT_EQ(joined.find("old low priority"), std::string::npos);
    EXPECT_NE(joined.find("new low priority"), std::string::npos);
    EXPECT_NE(joined.find("important"), std::string::npos);
    EXPECT_NE(joined.find("dropped 1 log messages"), std::string::npos);
}

TEST(LoggerTest, FullHighPriorityQueueDropsNewestRecord) {
    Logger logger;
    LoggerOptions options = SyncOptions();
    options.async = true;
    options.queue_capacity = 2;
    auto owned_sink = std::make_unique<BlockingSink>();
    BlockingSink* sink = owned_sink.get();
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::move(owned_sink));
    ASSERT_TRUE(logger.Initialize(options, std::move(sinks)));

    logger.Write(LogLevel::kError, "queue", "writer blocked");
    ASSERT_TRUE(sink->WaitUntilEntered(std::chrono::seconds(1)));
    logger.Write(LogLevel::kWarn, "queue", "first queued warning");
    logger.Write(LogLevel::kError, "queue", "second queued error");
    logger.Write(LogLevel::kError, "queue", "dropped newest error");

    sink->Release();
    logger.Shutdown();

    const auto lines = sink->Lines();
    ASSERT_EQ(lines.size(), 4u);
    const std::string joined = lines[0] + lines[1] + lines[2] + lines[3];
    EXPECT_NE(joined.find("first queued warning"), std::string::npos);
    EXPECT_NE(joined.find("second queued error"), std::string::npos);
    EXPECT_EQ(joined.find("dropped newest error"), std::string::npos);
    EXPECT_NE(joined.find("dropped 1 log messages"), std::string::npos);
}

}  // namespace
}  // namespace chat
