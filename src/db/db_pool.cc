#include "db/db_pool.h"

namespace chat {

DbPool::DbPool(const DbConfig& config) : config_(config) {}

bool DbPool::init() {
  return !config_.host.empty() && !config_.database.empty();
}

}  // namespace chat
