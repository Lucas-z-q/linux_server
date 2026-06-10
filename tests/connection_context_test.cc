#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

void TestConnectionMetadataTracksIndependentEvents() {
    int peer_fd = -1;
    ConnectionContext context = MakeContext(&peer_fd);
    const ConnectionMeta initial = context.meta();

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    context.touchOnRecv(7);
    assert(context.meta().last_recv_at > initial.last_recv_at);
    assert(context.meta().last_send_at == initial.last_send_at);
    assert(context.meta().last_active_at == initial.last_active_at);
    assert(context.meta().recv_count == 1);
    assert(context.meta().recv_bytes == 7);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::vector<std::string> packets;
    assert(context.feedPacketData("partial", packets));
    assert(packets.empty());
    assert(context.meta().last_active_at == initial.last_active_at);
    assert(context.feedPacketData("_packet\n", packets));
    assert(packets.size() == 1);
    assert(packets[0] == "partial_packet");
    assert(context.meta().last_active_at > initial.last_active_at);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    context.touchOnSend(11);
    assert(context.meta().last_send_at > initial.last_send_at);
    assert(context.meta().send_count == 1);
    assert(context.meta().sent_bytes == 11);

    context.setAuthenticatedUserId(10001);
    assert(context.meta().authenticated_user_id == 10001);
    context.clearAuthenticatedUserId();
    assert(context.meta().authenticated_user_id == 0);

    close(peer_fd);
}

void TestTimeoutDefersForPendingWork() {
    int peer_fd = -1;
    ConnectionContext context = MakeContext(&peer_fd);
    assert(!context.shouldDeferTimeout());

    context.appendPendingSend("response");
    assert(context.shouldDeferTimeout());
    context.clearPendingSend();
    assert(!context.shouldDeferTimeout());

    assert(context.startRequestOrQueue("request"));
    assert(context.shouldDeferTimeout());
    context.clearPendingRequests();
    assert(!context.shouldDeferTimeout());

    context.incrementPendingDeliveryMarks();
    assert(context.shouldDeferTimeout());
    context.decrementPendingDeliveryMarks();
    assert(!context.shouldDeferTimeout());

    close(peer_fd);
}

}  // namespace

int main() {
    TestQueuedRequestDoesNotBypassOlderRequestAfterBarrierClears();
    TestBarrierReleaseDoesNotClearNewInflightRequest();
    TestConnectionMetadataTracksIndependentEvents();
    TestTimeoutDefersForPendingWork();
    std::cout << "[PASS] connection context tests passed\n";
    return 0;
}
