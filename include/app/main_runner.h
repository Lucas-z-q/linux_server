#ifndef LINUX_SERVER_INCLUDE_APP_MAIN_RUNNER_H_
#define LINUX_SERVER_INCLUDE_APP_MAIN_RUNNER_H_

#include <functional>

using StartServerFn = std::function<bool()>;
using PostStartFn = std::function<void()>;

int RunMain(const StartServerFn& start_server, const PostStartFn& post_start_hook = {});

#endif  // LINUX_SERVER_INCLUDE_APP_MAIN_RUNNER_H_
