#include "app/main_runner.h"

#include <cerrno>
#include <cstring>

#include "common/logger.h"

int RunMain(const StartServerFn& start_server, const PostStartFn& post_start_hook) {
    if (!start_server()) {
        if (errno != 0) {
            LOG_ERROR("Main") << "server start failed errno=" << errno << " error=" << std::strerror(errno);
        } else {
            LOG_ERROR("Main") << "server start failed";
        }
        return 1;
    }

    if (post_start_hook) {
        post_start_hook();
    }

    return 0;
}
