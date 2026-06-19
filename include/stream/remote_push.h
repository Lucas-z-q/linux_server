#ifndef LINUX_SERVER_INCLUDE_STREAM_REMOTE_PUSH_H_
#define LINUX_SERVER_INCLUDE_STREAM_REMOTE_PUSH_H_

#include <string>

#include "common/types.h"

namespace chat {

struct RemotePushEvent {
    std::string event_id;
    std::string message_id;
    UserId from_user_id = 0;
    UserId to_user_id = 0;
    ConnectionId target_connection_id = 0;
    std::string payload;
};

enum class RemoteDeliveryOutcome {
    kDelivered,
    kInvalidTarget,
    kRetry,
};

class IRemotePushPublisher {
   public:
    virtual ~IRemotePushPublisher() = default;
    virtual bool Publish(const std::string &target_server_id, const RemotePushEvent &event) = 0;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_STREAM_REMOTE_PUSH_H_
