#ifndef LINUX_SERVER_INCLUDE_CONFIG_CONFIG_LOADER_H_
#define LINUX_SERVER_INCLUDE_CONFIG_CONFIG_LOADER_H_

#include <string>
#include <variant>

#include "config/server_config.h"

// 本文件声明配置加载器接口。
//
// 加载流程（固定顺序）：
//   结构体默认值 -> 读取 JSON -> 环境变量覆盖 -> 全量校验 -> 返回 ServerConfig
//
// 路径优先级：
//   --config 参数 > CHAT_CONFIG_PATH 环境变量 > config/server.json

namespace chat {

// 配置加载错误，携带字段路径以便快速定位。
struct ConfigError {
    // 人类可读的错误描述，格式如："mysql.pool.max_connections must be positive"
    std::string message;
};

// 加载结果：成功时持有 ServerConfig，失败时持有 ConfigError。
using ConfigResult = std::variant<ServerConfig, ConfigError>;

class ConfigLoader {
   public:
    // 从文件路径加载配置。
    // path 为空时按优先级自动探测：CHAT_CONFIG_PATH > config/server.json。
    static ConfigResult Load(const std::string& path = "");

    // 从 JSON 字符串加载配置（主要供测试使用）。
    static ConfigResult LoadFromString(const std::string& json_str);

   private:
    // 解析和校验逻辑的内部实现。
    // 先应用默认值，再叠加环境变量覆盖，最后执行全量校验。
    static ConfigResult ParseAndValidate(const std::string& json_str);

    // 用环境变量覆盖敏感字段（CHAT_DB_PASSWORD、CHAT_REDIS_PASSWORD 等）。
    static void ApplyEnvOverrides(ServerConfig& config);

    // 全量校验配置，返回首个校验错误；通过则返回空字符串。
    static std::string Validate(const ServerConfig& config);
};

// 便捷函数：从命令行参数解析 --config 值。
// 未找到时返回空字符串。
std::string ParseConfigPathArg(int argc, char* argv[]);

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CONFIG_CONFIG_LOADER_H_
