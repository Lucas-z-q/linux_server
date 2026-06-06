#include "redis/redis_client.h"

namespace chat {

RedisCommandResult RedisClient::Command(const std::vector<std::string> &args) {
    if (pool_ == nullptr) {
        return {.success = false, .error = RedisError::kConnectionUnavailable, .message = "redis pool is null"};
    }
    BorrowRedisConnectionResult borrowed = pool_->Borrow();
    if (!borrowed.ok()) {
        return {.success = false, .error = borrowed.error, .message = borrowed.message};
    }
    RedisCommandResult result = (*borrowed.connection)->Execute(args);
    if (result.error == RedisError::kConnectionUnavailable || result.error == RedisError::kTimeout) {
        borrowed.connection->MarkBad();
    }
    return result;
}

}  // namespace chat
