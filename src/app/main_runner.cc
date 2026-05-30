#include "app/main_runner.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

int RunMain(const StartServerFn& start_server, const PostStartFn& post_start_hook) {
    if (!start_server()) {
        if (errno != 0) {
            std::fprintf(stderr, "server start failed: %s\n", std::strerror(errno));
        } else {
            std::fputs("server start failed\n", stderr);
        }
        return 1;
    }

    if (post_start_hook) {
        post_start_hook();
    }

    return 0;
}
