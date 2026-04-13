#ifndef LINUX_SERVER_INCLUDE_CODEC_PACKET_CODEC_H_
#define LINUX_SERVER_INCLUDE_CODEC_PACKET_CODEC_H_

#include <string>
#include <vector>

// 本文件声明 TCP 应用层分包与封包工具。
// 当前阶段计划使用换行符分隔消息，后续可平滑演进到长度前缀协议。
//
// TODO(lzq): 将当前按换行分包的实现升级为长度前缀协议。
// TODO(lzq): 增加最大包长限制，防止异常数据撑爆缓冲区。
// TODO(lzq): 为非法包和半包场景补充单元测试。

namespace chat {

// 负责从字节流中切分完整业务包，并封装发送载荷。
class PacketCodec {
 public:
  // 喂入一段网络层收到的原始数据，并返回当前可解析出的完整消息。
  std::vector<std::string> feed(const std::string& chunk);

  // 将业务载荷编码成可直接下发到网络层的完整包。
  std::string encode(const std::string& payload) const;

 private:
  // 保存尚未形成完整包的残留字节。
  std::string buffer_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CODEC_PACKET_CODEC_H_
