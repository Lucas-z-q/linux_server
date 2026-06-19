#include <cstdlib>
#include <iostream>

#include "app/main_runner.h"

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << '\n';
        std::exit(1);
    }
}

void TestRunMainReturnsZeroAndRunsHook() {
    bool start_called = false;
    bool hook_called = false;

    const int rc = RunMain(
        [&]() {
            start_called = true;
            return true;
        },
        [&]() { hook_called = true; });

    Expect(start_called, "start callback should be called on success path");
    Expect(hook_called, "post-start hook should run after successful start");
    Expect(rc == 0, "RunMain should return 0 when server startup succeeds");
}

void TestRunMainReturnsOneAndSkipsHookOnFailure() {
    bool start_called = false;
    bool hook_called = false;

    const int rc = RunMain(
        [&]() {
            start_called = true;
            return false;
        },
        [&]() { hook_called = true; });

    Expect(start_called, "start callback should be called on failure path");
    Expect(!hook_called, "post-start hook should not run when startup fails");
    Expect(rc == 1, "RunMain should return 1 when server startup fails");
}

}  // namespace

int main() {
    TestRunMainReturnsZeroAndRunsHook();
    TestRunMainReturnsOneAndSkipsHookOnFailure();
    std::cout << "[PASS] main control flow tests passed\n";
    return 0;
}
