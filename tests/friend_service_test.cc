#include "service/friend_service.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include "fake_friend_repository.h"
#include "fake_user_repository.h"

namespace {

class FakeSessionManager : public chat::ISessionManager {
   public:
    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions;
    std::unordered_map<chat::UserId, chat::ConnectionId> user_conns;

    bool BindSession(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
        sessions[connection_id] = session;
        if (session.authenticated) {
            user_conns[session.user_id] = connection_id;
        }
        return true;
    }

    std::optional<chat::ConnectionId> GetConnectionId(chat::UserId user_id) override {
        const auto it = user_conns.find(user_id);
        if (it == user_conns.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<chat::ConnectionSession> GetSession(chat::ConnectionId connection_id) override {
        const auto it = sessions.find(connection_id);
        if (it == sessions.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void ClearSession(chat::ConnectionId connection_id) override {
        const auto it = sessions.find(connection_id);
        if (it != sessions.end()) {
            user_conns.erase(it->second.user_id);
            sessions.erase(it);
        }
    }
};

void BindUser(FakeSessionManager* session_manager, chat::ConnectionId conn_id, chat::UserId user_id,
              const std::string& username) {
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = user_id;
    session.username = username;
    session_manager->BindSession(conn_id, session);
}

void TestAddFriendCreatesPendingRequest() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");

    chat::AddFriendRequest req;
    req.target_user_id = 2;
    const chat::FriendshipActionResult result = service.addFriend(100, req);

    assert(result.code == chat::ErrorCode::OK);
    assert(result.friendship.has_value());
    assert(result.friendship->requester_id == 1);
    assert(result.friendship->addressee_id == 2);
    assert(result.friendship->status == chat::FriendshipStatus::kPending);
    assert(friend_repo.create_calls == 1);
}

void TestAddFriendRejectsDuplicatePendingRequest() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    chat::FriendshipRecord pending;
    pending.id = 1;
    pending.requester_id = 1;
    pending.addressee_id = 2;
    pending.status = chat::FriendshipStatus::kPending;
    friend_repo.records.push_back(pending);
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");

    chat::AddFriendRequest req;
    req.target_user_id = 2;
    const chat::FriendshipActionResult result = service.addFriend(100, req);

    assert(result.code == chat::ErrorCode::FRIEND_REQUEST_ALREADY_EXISTS);
    assert(friend_repo.create_calls == 0);
}

void TestAcceptFriendChangesPendingToAccepted() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    chat::FriendshipRecord pending;
    pending.id = 1;
    pending.requester_id = 1;
    pending.addressee_id = 2;
    pending.status = chat::FriendshipStatus::kPending;
    friend_repo.records.push_back(pending);
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 200, 2, "user2");

    chat::AcceptFriendRequest req;
    req.requester_user_id = 1;
    const chat::FriendshipActionResult result = service.acceptFriend(200, req);

    assert(result.code == chat::ErrorCode::OK);
    assert(result.friendship.has_value());
    assert(result.friendship->status == chat::FriendshipStatus::kAccepted);
    assert(friend_repo.accept_calls == 1);
}

void TestRequesterCannotAcceptOwnOutboundRequest() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    chat::FriendshipRecord pending;
    pending.id = 1;
    pending.requester_id = 1;
    pending.addressee_id = 2;
    pending.status = chat::FriendshipStatus::kPending;
    friend_repo.records.push_back(pending);
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");

    chat::AcceptFriendRequest req;
    req.requester_user_id = 2;
    const chat::FriendshipActionResult result = service.acceptFriend(100, req);

    assert(result.code == chat::ErrorCode::FRIEND_REQUEST_NOT_FOUND);
    assert(friend_repo.accept_calls == 0);
}

void TestAcceptFriendQueryFailureReturnsDbError() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    friend_repo.find_status = chat::RepositoryStatus::kQueryFailed;
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 200, 2, "user2");

    chat::AcceptFriendRequest req;
    req.requester_user_id = 1;
    const chat::FriendshipActionResult result = service.acceptFriend(200, req);

    assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
    assert(friend_repo.accept_calls == 0);
}

void TestAcceptFriendUpdateFailureReturnsDbError() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    chat::FriendshipRecord pending;
    pending.id = 1;
    pending.requester_id = 1;
    pending.addressee_id = 2;
    pending.status = chat::FriendshipStatus::kPending;
    friend_repo.records.push_back(pending);
    friend_repo.accept_status = chat::RepositoryStatus::kQueryFailed;
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 200, 2, "user2");

    chat::AcceptFriendRequest req;
    req.requester_user_id = 1;
    const chat::FriendshipActionResult result = service.acceptFriend(200, req);

    assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
    assert(friend_repo.accept_calls == 1);
}

void TestDeleteFriendshipRemovesRelation() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    chat::FriendshipRecord accepted;
    accepted.id = 1;
    accepted.requester_id = 1;
    accepted.addressee_id = 2;
    accepted.status = chat::FriendshipStatus::kAccepted;
    friend_repo.records.push_back(accepted);
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");

    chat::DeleteFriendRequest req;
    req.friend_user_id = 2;
    const chat::FriendshipActionResult result = service.deleteFriend(100, req);

    assert(result.code == chat::ErrorCode::OK);
    assert(friend_repo.records.empty());
    assert(friend_repo.delete_calls == 1);
}

void TestDeleteFriendFailureReturnsDbError() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    friend_repo.delete_status = chat::RepositoryStatus::kQueryFailed;
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");

    chat::DeleteFriendRequest req;
    req.friend_user_id = 2;
    const chat::FriendshipActionResult result = service.deleteFriend(100, req);

    assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
    assert(friend_repo.delete_calls == 1);
}

void TestListFriendsReturnsStatusAndDirection() {
    FakeSessionManager session_manager;
    FakeUserRepository user_repo;
    FakeFriendRepository friend_repo;
    chat::FriendshipRecord pending;
    pending.id = 1;
    pending.requester_id = 1;
    pending.addressee_id = 2;
    pending.status = chat::FriendshipStatus::kPending;
    pending.created_at = "2026-06-16 00:00:00";
    pending.updated_at = "2026-06-16 00:00:00";
    friend_repo.records.push_back(pending);
    chat::FriendService service(session_manager, friend_repo, user_repo);
    BindUser(&session_manager, 200, 2, "user2");

    const chat::ListFriendsResult result = service.listFriends(200);

    assert(result.code == chat::ErrorCode::OK);
    assert(result.friends.size() == 1);
    assert(result.friends[0].user_id == 1);
    assert(result.friends[0].status == chat::FriendshipStatus::kPending);
    assert(result.friends[0].direction == "incoming");
    assert(friend_repo.list_calls == 1);
}

}  // namespace

int main() {
    TestAddFriendCreatesPendingRequest();
    TestAddFriendRejectsDuplicatePendingRequest();
    TestAcceptFriendChangesPendingToAccepted();
    TestRequesterCannotAcceptOwnOutboundRequest();
    TestAcceptFriendQueryFailureReturnsDbError();
    TestAcceptFriendUpdateFailureReturnsDbError();
    TestDeleteFriendshipRemovesRelation();
    TestDeleteFriendFailureReturnsDbError();
    TestListFriendsReturnsStatusAndDirection();
    std::cout << "[PASS] friend service tests passed\n";
    return 0;
}
