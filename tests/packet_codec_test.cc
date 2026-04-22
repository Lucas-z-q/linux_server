#include "codec/packet_codec.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace chat;

namespace {

void TestEncodeAppendsNewline() {
  PacketCodec codec;
  const std::string payload = R"({"msg_type":"heartbeat","seq":1,"data":{}})";
  const std::string encoded = codec.encode(payload);

  assert(encoded == payload + "\n");
}

void TestFeedReturnsMultiplePacketsFromSingleChunk() {
  PacketCodec codec;
  std::vector<std::string> packets;
  const bool ok = codec.feed("first\nsecond\n", packets);

  assert(ok);
  assert(packets.size() == 2);
  assert(packets[0] == "first");
  assert(packets[1] == "second");
}

void TestFeedBuffersHalfPacketUntilDelimiterArrives() {
  PacketCodec codec;
  std::vector<std::string> packets;

  const bool first_ok = codec.feed("part", packets);
  assert(first_ok);
  assert(packets.empty());

  const bool second_ok = codec.feed("ial\n", packets);
  assert(second_ok);
  assert(packets.size() == 1);
  assert(packets[0] == "partial");
}

void TestFeedReturnsEmptyPacketForBlankLine() {
  PacketCodec codec;
  std::vector<std::string> packets;
  const bool ok = codec.feed("\n", packets);

  assert(ok);
  assert(packets.size() == 1);
  assert(packets[0].empty());
}

void TestFeedStripsTrailingCarriageReturnForCrlfPackets() {
  PacketCodec codec;
  std::vector<std::string> packets;

  const bool ok = codec.feed("first\r\nsecond\r\n", packets);

  assert(ok);
  assert(packets.size() == 2);
  assert(packets[0] == "first");
  assert(packets[1] == "second");
}

void TestFeedRejectsOversizedBufferedPacket() {
  PacketCodec codec;
  std::vector<std::string> packets;
  const std::string oversized(PacketCodec::kMaxPacketSize + 1, 'x');

  const bool ok = codec.feed(oversized, packets);

  assert(!ok);
  assert(packets.empty());

  const bool recovery_ok = codec.feed("ok\n", packets);
  assert(recovery_ok);
  assert(packets.size() == 1);
  assert(packets[0] == "ok");
}

}  // namespace

int main() {
  TestEncodeAppendsNewline();
  TestFeedReturnsMultiplePacketsFromSingleChunk();
  TestFeedBuffersHalfPacketUntilDelimiterArrives();
  TestFeedReturnsEmptyPacketForBlankLine();
  TestFeedStripsTrailingCarriageReturnForCrlfPackets();
  TestFeedRejectsOversizedBufferedPacket();
  std::cout << "[PASS] packet codec tests passed\n";
  return 0;
}
