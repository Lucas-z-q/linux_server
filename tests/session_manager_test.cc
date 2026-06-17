#include "server/session_manager.h"

#include <cassert>
#include <iostream>

namespace {

void TestBindAndGetSession() {
    chat::SessionManager manager;
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 10001;
    session.username = "alice";
    session.token = "token_10001";

    const bool bind_ok = manager.BindSession(42, session);
    assert(bind_ok);

    const auto saved = manager.GetSession(42);
    assert(saved.has_value());
    assert(saved->authenticated);
    assert(saved->user_id == 10001);
    assert(saved->username == "alice");
    assert(saved->token == "token_10001");

    const auto conn_id = manager.GetConnectionId(10001);
    assert(conn_id.has_value());
    assert(*conn_id == 42);
}

void TestBindingSameUserKeepsMultipleConnections() {
    chat::SessionManager manager;
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 10001;
    session.username = "alice";
    session.token = "token_10001";

    assert(manager.BindSession(42, session));
    assert(manager.BindSession(77, session));

    assert(manager.GetSession(42).has_value());
    const auto rebound = manager.GetSession(77);
    assert(rebound.has_value());
    assert(rebound->user_id == 10001);
    const auto connections = manager.GetConnectionIds(10001);
    assert(connections.size() == 2);
    assert(connections[0] == 42);
    assert(connections[1] == 77);
}

void TestClearSessionRemovesIndexes() {
    chat::SessionManager manager;
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 10001;
    session.username = "alice";
    session.token = "token_10001";

    assert(manager.BindSession(42, session));
    manager.ClearSession(42);

    assert(!manager.GetSession(42).has_value());
    assert(!manager.GetConnectionId(10001).has_value());
}

void TestClearOneDeviceKeepsOtherDevice() {
    chat::SessionManager manager;
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 10001;
    session.username = "alice";
    session.token = "token_10001";

    assert(manager.BindSession(42, session));
    assert(manager.BindSession(77, session));
    manager.ClearSession(42);

    assert(!manager.GetSession(42).has_value());
    assert(manager.GetSession(77).has_value());
    const auto connections = manager.GetConnectionIds(10001);
    assert(connections.size() == 1);
    assert(connections[0] == 77);
}

}  // namespace

int main() {
    TestBindAndGetSession();
    TestBindingSameUserKeepsMultipleConnections();
    TestClearSessionRemovesIndexes();
    TestClearOneDeviceKeepsOtherDevice();
    std::cout << "[PASS] session manager tests passed\n";
    return 0;
}
