#include "codec/packet_codec.h"

#include <utility>

namespace chat {

bool PacketCodec::feed(const std::string& chunk, std::vector<std::string>& packets) {
    packets.clear();
    buffer_.append(chunk);

    std::size_t pos = 0;
    while ((pos = buffer_.find('\n')) != std::string::npos) {
        if (pos > kMaxPacketSize) {
            buffer_.clear();
            packets.clear();
            return false;
        }
        std::string packet = buffer_.substr(0, pos);
        if (!packet.empty() && packet.back() == '\r') {
            packet.pop_back();
        }
        packets.push_back(std::move(packet));
        buffer_.erase(0, pos + 1);
    }
    if (buffer_.size() > kMaxPacketSize) {
        buffer_.clear();
        packets.clear();
        return false;
    }
    return true;
}

std::string PacketCodec::encode(const std::string& payload) const { return payload + "\n"; }

}  // namespace chat
