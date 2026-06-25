#ifndef LINUX_SERVER_INCLUDE_CODEC_PACKET_CODEC_H_
#define LINUX_SERVER_INCLUDE_CODEC_PACKET_CODEC_H_

#include <cstddef>
#include <string>
#include <vector>

// PacketCodec 使用换行符分隔应用层消息，并限制单个消息的最大缓存大小。

namespace chat {

// 负责从字节流中切分完整业务包，并封装发送载荷。
class PacketCodec {
   public:
    // 当前按换行协议允许缓存的最大半包大小。
    static constexpr std::size_t kMaxPacketSize = 64 * 1024;

    // 喂入一段网络层收到的原始数据，并返回当前可解析出的完整消息。
    // 返回 false 表示缓冲超出上限，调用方应视为协议异常并关闭连接。
    bool feed(const std::string &chunk, std::vector<std::string> &packets);

    // 将业务载荷编码成可直接下发到网络层的完整包。
    std::string encode(const std::string &payload) const;

   private:
    // 保存尚未形成完整包的残留字节。
    std::string buffer_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CODEC_PACKET_CODEC_H_
