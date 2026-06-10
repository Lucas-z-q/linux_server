#include "common/logger.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace chat {

namespace {

class ConsoleSink final : public ILogSink {
   public:
    bool Write(std::string_view line) noexcept override {
        if (std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data()) < 0) {
            return false;
        }
        return std::fflush(stderr) == 0;
    }

    void Flush() noexcept override { std::fflush(stderr); }
};

class RollingFileSink final : public ILogSink {
   public:
    RollingFileSink(std::string path, std::size_t max_file_size_bytes, std::size_t max_files)
        : path_(std::move(path)), max_file_size_bytes_(max_file_size_bytes), max_files_(max_files) {}

    bool Initialize() noexcept {
        try {
            const std::filesystem::path path(path_);
            const std::filesystem::path parent = path.parent_path();
            std::error_code error;
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, error);
                if (error) {
                    return false;
                }
            }

            current_size_ = 0;
            if (std::filesystem::exists(path, error)) {
                if (error) {
                    return false;
                }
                current_size_ = static_cast<std::size_t>(std::filesystem::file_size(path, error));
                if (error) {
                    return false;
                }
            }
            return Open(std::ios::app);
        } catch (...) {
            return false;
        }
    }

    bool Write(std::string_view line) noexcept override {
        try {
            const std::size_t write_size = line.size() + 1;
            if (current_size_ > 0 &&
                write_size > max_file_size_bytes_ - std::min(current_size_, max_file_size_bytes_)) {
                if (!Roll()) {
                    return false;
                }
            }

            stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
            stream_.put('\n');
            if (!stream_) {
                return false;
            }
            current_size_ += write_size;
            return true;
        } catch (...) {
            return false;
        }
    }

    void Flush() noexcept override {
        try {
            stream_.flush();
        } catch (...) {
        }
    }

   private:
    bool Open(std::ios::openmode mode) {
        stream_.open(path_, std::ios::out | mode);
        return stream_.is_open();
    }

    bool Roll() {
        stream_.flush();
        stream_.close();

        std::error_code error;
        if (max_files_ == 1) {
            current_size_ = 0;
            return Open(std::ios::trunc);
        }

        const std::filesystem::path current(path_);
        std::filesystem::remove(path_ + "." + std::to_string(max_files_ - 1), error);
        error.clear();

        for (std::size_t index = max_files_ - 1; index > 1; --index) {
            const std::filesystem::path source = path_ + "." + std::to_string(index - 1);
            const std::filesystem::path destination = path_ + "." + std::to_string(index);
            if (!std::filesystem::exists(source, error)) {
                if (error) {
                    return false;
                }
                continue;
            }
            std::filesystem::rename(source, destination, error);
            if (error) {
                return false;
            }
        }

        if (std::filesystem::exists(current, error)) {
            if (error) {
                return false;
            }
            std::filesystem::rename(current, path_ + ".1", error);
            if (error) {
                return false;
            }
        }

        current_size_ = 0;
        return Open(std::ios::trunc);
    }

    std::string path_;
    std::size_t max_file_size_bytes_;
    std::size_t max_files_;
    std::size_t current_size_ = 0;
    std::ofstream stream_;
};

std::string EscapeLine(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\r') {
            escaped.append("\\r");
        } else if (ch == '\n') {
            escaped.append("\\n");
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

std::string FormatLine(LogLevel level, std::string_view module, std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time{};
    gmtime_r(&now_time, &utc_time);

    std::ostringstream line;
    line << '[' << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
         << milliseconds << "Z] [tid=" << static_cast<std::int64_t>(syscall(SYS_gettid)) << "] ["
         << LogLevelToString(level) << "] [" << EscapeLine(module) << "] " << EscapeLine(message);
    return line.str();
}

bool IsLowPriority(LogLevel level) { return level == LogLevel::kDebug || level == LogLevel::kInfo; }

bool ValidateOptions(const LoggerOptions& options) {
    return options.max_file_size_bytes > 0 && options.max_files > 0 && options.queue_capacity > 0;
}

void EmergencyWrite(std::string_view message) noexcept {
    std::fprintf(stderr, "[Logger] %.*s\n", static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

}  // namespace

const char* LogLevelToString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kDebug:
            return "DEBUG";
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarn:
            return "WARN";
        case LogLevel::kError:
            return "ERROR";
    }
    return "UNKNOWN";
}

bool ParseLogLevel(std::string_view value, LogLevel* level) noexcept {
    if (level == nullptr) {
        return false;
    }
    if (value == "debug") {
        *level = LogLevel::kDebug;
    } else if (value == "info") {
        *level = LogLevel::kInfo;
    } else if (value == "warn") {
        *level = LogLevel::kWarn;
    } else if (value == "error") {
        *level = LogLevel::kError;
    } else {
        return false;
    }
    return true;
}

Logger::Logger() { sinks_.push_back(std::make_unique<ConsoleSink>()); }

Logger::~Logger() { Shutdown(); }

Logger& Logger::Instance() {
    static Logger logger;
    return logger;
}

bool Logger::Initialize(const LoggerOptions& options) {
    if (!ValidateOptions(options) || (!options.console && options.file_path.empty())) {
        return false;
    }

    std::vector<std::unique_ptr<ILogSink>> sinks;
    if (options.console) {
        sinks.push_back(std::make_unique<ConsoleSink>());
    }
    if (!options.file_path.empty()) {
        auto file_sink =
            std::make_unique<RollingFileSink>(options.file_path, options.max_file_size_bytes, options.max_files);
        if (!file_sink->Initialize()) {
            EmergencyWrite("failed to initialize rolling file sink: " + options.file_path);
            return false;
        }
        sinks.push_back(std::move(file_sink));
    }
    return InitializeWithSinks(options, std::move(sinks));
}

bool Logger::Initialize(const LoggerOptions& options, std::vector<std::unique_ptr<ILogSink>> sinks) {
    if (!ValidateOptions(options) || sinks.empty()) {
        return false;
    }
    for (const auto& sink : sinks) {
        if (sink == nullptr) {
            return false;
        }
    }
    return InitializeWithSinks(options, std::move(sinks));
}

bool Logger::InitializeWithSinks(const LoggerOptions& options, std::vector<std::unique_ptr<ILogSink>> sinks) {
    Shutdown();

    {
        std::lock_guard<std::mutex> lock(sinks_mutex_);
        sinks_ = std::move(sinks);
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.clear();
        queued_logs_ = 0;
        dropped_logs_ = 0;
        queue_capacity_ = options.queue_capacity;
        stop_requested_ = false;
        worker_running_ = false;
    }

    min_level_.store(options.min_level);
    async_mode_.store(options.async);
    accepting_.store(true);

    if (!options.async) {
        return true;
    }

    try {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            worker_running_ = true;
        }
        worker_ = std::thread(&Logger::WorkerLoop, this);
    } catch (const std::exception& exception) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            worker_running_ = false;
            stop_requested_ = true;
        }
        accepting_.store(false);
        async_mode_.store(false);
        EmergencyWrite(std::string("failed to start logger thread: ") + exception.what());
        return false;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            worker_running_ = false;
            stop_requested_ = true;
        }
        accepting_.store(false);
        async_mode_.store(false);
        EmergencyWrite("failed to start logger thread");
        return false;
    }
    return true;
}

bool Logger::ShouldLog(LogLevel level) const noexcept {
    return accepting_.load() && static_cast<int>(level) >= static_cast<int>(min_level_.load());
}

void Logger::Write(LogLevel level, std::string_view module, std::string message) noexcept {
    if (!ShouldLog(level) || !accepting_.load()) {
        return;
    }

    try {
        std::string line = FormatLine(level, module, message);
        if (!async_mode_.load()) {
            WriteToSinks(line);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!accepting_.load() || stop_requested_) {
                return;
            }

            if (queued_logs_ >= queue_capacity_) {
                if (IsLowPriority(level)) {
                    ++dropped_logs_;
                    return;
                }

                auto low_priority = queue_.end();
                for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                    if (it->type == QueueItem::Type::kLog && IsLowPriority(it->level)) {
                        low_priority = it;
                        break;
                    }
                }
                if (low_priority == queue_.end()) {
                    ++dropped_logs_;
                    return;
                }
                queue_.erase(low_priority);
                --queued_logs_;
                ++dropped_logs_;
            }

            QueueItem item;
            item.type = QueueItem::Type::kLog;
            item.level = level;
            item.line = std::move(line);
            queue_.push_back(std::move(item));
            ++queued_logs_;
        }
        queue_cv_.notify_one();
    } catch (...) {
        EmergencyWrite("failed to format or enqueue log message");
    }
}

void Logger::Flush() noexcept {
    try {
        if (!async_mode_.load()) {
            FlushSinks();
            return;
        }

        auto barrier = std::make_shared<FlushBarrier>();
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!worker_running_) {
                FlushSinks();
                return;
            }
            QueueItem item;
            item.type = QueueItem::Type::kFlush;
            item.barrier = barrier;
            queue_.push_back(std::move(item));
        }
        queue_cv_.notify_one();

        std::unique_lock<std::mutex> lock(barrier->mutex);
        barrier->cv.wait(lock, [&barrier]() { return barrier->done; });
    } catch (...) {
        EmergencyWrite("logger flush failed");
    }
}

void Logger::Shutdown() noexcept {
    accepting_.store(false);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (worker_running_) {
            stop_requested_ = true;
        }
    }
    queue_cv_.notify_all();

    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (...) {
            EmergencyWrite("failed to join logger thread");
        }
    }

    async_mode_.store(false);
    FlushSinks();
}

void Logger::WorkerLoop() noexcept {
    while (true) {
        QueueItem item;
        std::size_t dropped = 0;
        bool has_item = false;
        bool should_stop = false;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return stop_requested_ || !queue_.empty() || dropped_logs_ > 0; });

            dropped = dropped_logs_;
            dropped_logs_ = 0;
            if (!queue_.empty()) {
                item = std::move(queue_.front());
                queue_.pop_front();
                if (item.type == QueueItem::Type::kLog) {
                    --queued_logs_;
                }
                has_item = true;
            } else if (stop_requested_) {
                worker_running_ = false;
                should_stop = true;
            }
        }

        if (dropped > 0) {
            WriteDroppedSummary(dropped);
        }
        if (should_stop) {
            break;
        }
        if (!has_item) {
            continue;
        }

        if (item.type == QueueItem::Type::kLog) {
            WriteToSinks(item.line);
        } else {
            FlushSinks();
            std::lock_guard<std::mutex> lock(item.barrier->mutex);
            item.barrier->done = true;
            item.barrier->cv.notify_all();
        }
    }

    FlushSinks();
}

void Logger::WriteToSinks(std::string_view line) noexcept {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (const auto& sink : sinks_) {
        bool ok = false;
        try {
            ok = sink->Write(line);
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            EmergencyWrite("log sink write failed");
        }
    }
}

void Logger::FlushSinks() noexcept {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (const auto& sink : sinks_) {
        try {
            sink->Flush();
        } catch (...) {
            EmergencyWrite("log sink flush failed");
        }
    }
}

void Logger::WriteDroppedSummary(std::size_t dropped) noexcept {
    try {
        WriteToSinks(
            FormatLine(LogLevel::kWarn, "Logger",
                       "dropped " + std::to_string(dropped) + " log messages because the async queue was full"));
    } catch (...) {
        EmergencyWrite("failed to report dropped log messages");
    }
}

LogMessage::LogMessage(Logger& logger, LogLevel level, std::string_view module) : logger_(logger), level_(level) {
    try {
        module_.assign(module.data(), module.size());
    } catch (...) {
        module_ = "Logger";
    }
}

LogMessage::~LogMessage() noexcept {
    try {
        logger_.Write(level_, module_, stream_.str());
    } catch (...) {
        EmergencyWrite("failed to submit log message");
    }
}

std::ostream& LogMessage::stream() noexcept { return stream_; }

}  // namespace chat
