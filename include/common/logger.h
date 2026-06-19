#ifndef LINUX_SERVER_INCLUDE_COMMON_LOGGER_H_
#define LINUX_SERVER_INCLUDE_COMMON_LOGGER_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace chat {

enum class LogLevel {
    kDebug = 0,
    kInfo,
    kWarn,
    kError,
};

const char* LogLevelToString(LogLevel level) noexcept;
bool ParseLogLevel(std::string_view value, LogLevel* level) noexcept;

struct LoggerOptions {
    LogLevel min_level = LogLevel::kInfo;
    bool console = true;
    std::string file_path;
    std::size_t max_file_size_bytes = 100 * 1024 * 1024;
    std::size_t max_files = 5;
    bool async = true;
    std::size_t queue_capacity = 8192;
};

class ILogSink {
   public:
    virtual ~ILogSink() = default;

    virtual bool Write(std::string_view line) noexcept = 0;
    virtual void Flush() noexcept = 0;
};

class Logger {
   public:
    Logger();
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& Instance();

    bool Initialize(const LoggerOptions& options);
    bool Initialize(const LoggerOptions& options, std::vector<std::unique_ptr<ILogSink>> sinks);
    bool ShouldLog(LogLevel level) const noexcept;
    void Write(LogLevel level, std::string_view module, std::string message) noexcept;
    void Flush() noexcept;
    void Shutdown() noexcept;

   private:
    struct FlushBarrier {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };

    struct QueueItem {
        enum class Type {
            kLog,
            kFlush,
        };

        Type type = Type::kLog;
        LogLevel level = LogLevel::kInfo;
        std::string line;
        std::shared_ptr<FlushBarrier> barrier;
    };

    bool InitializeWithSinks(const LoggerOptions& options, std::vector<std::unique_ptr<ILogSink>> sinks);
    void WorkerLoop() noexcept;
    void WriteToSinks(std::string_view line) noexcept;
    void FlushSinks() noexcept;
    void WriteDroppedSummary(std::size_t dropped) noexcept;

    std::atomic<LogLevel> min_level_{LogLevel::kInfo};
    std::atomic<bool> accepting_{true};
    std::atomic<bool> async_mode_{false};

    std::mutex sinks_mutex_;
    std::vector<std::unique_ptr<ILogSink>> sinks_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<QueueItem> queue_;
    std::size_t queued_logs_ = 0;
    std::size_t queue_capacity_ = 8192;
    std::size_t dropped_logs_ = 0;
    bool stop_requested_ = false;
    bool worker_running_ = false;
    std::thread worker_;
};

class LogMessage {
   public:
    LogMessage(Logger& logger, LogLevel level, std::string_view module);
    ~LogMessage() noexcept;

    LogMessage(const LogMessage&) = delete;
    LogMessage& operator=(const LogMessage&) = delete;

    std::ostream& stream() noexcept;

   private:
    Logger& logger_;
    LogLevel level_;
    std::string module_;
    std::ostringstream stream_;
};

class LogMessageVoidify {
   public:
    void operator&(std::ostream&) noexcept {}
};

}  // namespace chat

#define CHAT_LOG_STREAM(level, module)           \
    !::chat::Logger::Instance().ShouldLog(level) \
        ? (void)0                                \
        : ::chat::LogMessageVoidify() & ::chat::LogMessage(::chat::Logger::Instance(), level, module).stream()

#define LOG_DEBUG(module) CHAT_LOG_STREAM(::chat::LogLevel::kDebug, module)
#define LOG_INFO(module) CHAT_LOG_STREAM(::chat::LogLevel::kInfo, module)
#define LOG_WARN(module) CHAT_LOG_STREAM(::chat::LogLevel::kWarn, module)
#define LOG_ERROR(module) CHAT_LOG_STREAM(::chat::LogLevel::kError, module)

#endif  // LINUX_SERVER_INCLUDE_COMMON_LOGGER_H_
