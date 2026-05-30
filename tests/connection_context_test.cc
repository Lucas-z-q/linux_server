#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <iostream>
#include <string>

#include "net/ConnectionContext.h"

namespace {

ConnectionContext MakeContext(int* peer_fd) {
    int fds[2] = {-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    *peer_fd = fds[1];
    return ConnectionContext(TcpConnection(fds[0], "local", 0), 1);
}

void TestQueuedRequestDoesNotBypassOlderRequestAfterBarrierClears() {
    int peer_fd = -1;
    ConnectionContext context = MakeContext(&peer_fd);

    assert(context.startRequestOrQueue("current"));
    context.incrementPendingDeliveryMarks();

    std::string next_request;
    assert(!context.finishRequestAndPopNext(next_request));
    assert(!context.startRequestOrQueue("queued_old"));

    context.decrementPendingDeliveryMarks();

    // A packet parsed before the I/O thread processes the barrier-release event must stay behind
    // the already queued request for this connection.
    assert(!context.startRequestOrQueue("queued_new"));

    assert(context.popNextRequestIfIdle(next_request));
    assert(next_request == "queued_old");
    assert(!context.popNextRequestIfIdle(next_request));

    assert(context.finishRequestAndPopNext(next_request));
    assert(next_request == "queued_new");

    close(peer_fd);
}

void TestBarrierReleaseDoesNotClearNewInflightRequest() {
    int peer_fd = -1;
    ConnectionContext context = MakeContext(&peer_fd);

    context.incrementPendingDeliveryMarks();
    context.decrementPendingDeliveryMarks();

    // This simulates a new request starting in the small window before the I/O thread drains the
    // delivery-complete notification. A barrier-release event must not finish that new request.
    assert(context.startRequestOrQueue("new_active"));

    std::string next_request;
    assert(!context.popNextRequestIfIdle(next_request));
    assert(!context.startRequestOrQueue("queued_after_active"));

    assert(context.finishRequestAndPopNext(next_request));
    assert(next_request == "queued_after_active");

    close(peer_fd);
}

}  // namespace

int main() {
    TestQueuedRequestDoesNotBypassOlderRequestAfterBarrierClears();
    TestBarrierReleaseDoesNotClearNewInflightRequest();
    std::cout << "[PASS] connection context tests passed\n";
    return 0;
}
