#ifndef LINUX_SERVER_INCLUDE_NET_IMESSAGE_HANDLER_H_
#define LINUX_SERVER_INCLUDE_NET_IMESSAGE_HANDLER_H_

#include <string>

// 本文件定义网络层与业务处理层之间的最小接口。
// TcpServer 只依赖这个抽象，从而避免与具体业务实现耦合。
//
// TODO(lzq): 在接口中加入连接上下文参数，支持按连接处理认证态。
// TODO(lzq): 明确 handle 的异常约束，统一使用返回值还是错误对象。
// TODO(lzq): 如果后续需要异步业务处理，可补充回调或任务投递接口。

// 抽象一类“输入请求字符串，输出响应字符串”的消息处理器。
class IMessageHandler {
 public:
  // 允许通过基类指针安全析构派生类。
  virtual ~IMessageHandler() = default;

  // 处理一条请求报文，并返回要发送给客户端的响应报文。
  virtual std::string handle(const std::string& request) = 0;
};

#endif  // LINUX_SERVER_INCLUDE_NET_IMESSAGE_HANDLER_H_
