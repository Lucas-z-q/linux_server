#ifndef LINUX_SERVER_INCLUDE_DB_DB_POOL_H_
#define LINUX_SERVER_INCLUDE_DB_DB_POOL_H_

#include "config/db_config.h"

// 本文件声明数据库连接池抽象。
// 当前阶段可以先提供轻量封装，后续再补充连接复用与线程安全实现。
//
// TODO(lzq): 明确连接池使用的底层 MySQL C++ 客户端库。
// TODO(lzq): 为连接借出、归还和销毁补充完整接口。
// TODO(lzq): 增加连接健康检查和自动重连策略。

namespace chat {

// 负责初始化并管理数据库连接资源。
class DbPool {
 public:
  // 使用给定配置构造数据库连接池对象。
  explicit DbPool(const DbConfig& config);

  // 初始化底层连接资源。
  bool init();

 private:
  // 数据库初始化配置快照。
  DbConfig config_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_DB_POOL_H_
