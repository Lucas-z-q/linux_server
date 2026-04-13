#include "codec/packet_codec.h"

#include <utility>

namespace chat {

std::vector<std::string> PacketCodec::feed(const std::string& chunk) {
  buffer_.append(chunk);
  std::vector<std::string> packets;

  std::size_t pos = 0;
  while ((pos = buffer_.find('\n')) != std::string::npos) {
    packets.push_back(buffer_.substr(0, pos));
    buffer_.erase(0, pos + 1);
  }
  return packets;
}

std::string PacketCodec::encode(const std::string& payload) const {
  return payload + "\n";
}

}  // namespace chat
