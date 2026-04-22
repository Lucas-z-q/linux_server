#ifndef LINUX_SERVER_INCLUDE_SERVER_CONNECTION_MANAGER_H_
#define LINUX_SERVER_INCLUDE_SERVER_CONNECTION_MANAGER_H_

// 本文件预留连接管理器接口位置。
// 后续如果连接元数据、在线状态和广播能力膨胀，可以从 TcpServer 中抽离。
//
// TODO(lzq): 定义连接注册、查询和移除接口。
// TODO(lzq): 明确该模块与 TcpServer、SessionManager 的职责边界。
// TODO(lzq): 评估是否需要支持按用户维度查找连接集合。

namespace chat {

// 预留的连接管理器占位类型。
class ConnectionManager {};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVER_CONNECTION_MANAGER_H_
