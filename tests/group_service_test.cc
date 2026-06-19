#include "service/group_service.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include "fake_group_repository.h"
#include "fake_message_repository.h"
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

chat::GroupRecord AddGroup(FakeGroupRepository* group_repo, const std::string& group_id, chat::UserId owner_id,
                           const std::vector<chat::UserId>& member_ids) {
    chat::GroupRecord group;
    group.id = group_id;
    group.name = "team";
    group.owner_id = owner_id;
    group.conversation_id = "gconv_" + group_id;
    group_repo->groups[group_id] = group;
    for (const chat::UserId user_id : member_ids) {
        chat::GroupMemberRecord member;
        member.group_id = group_id;
        member.user_id = user_id;
        member.role = user_id == owner_id ? "owner" : "member";
        group_repo->members[group_id].push_back(member);
    }
    return group;
}

void TestCreateGroupAddsOwnerAndMembers() {
    FakeSessionManager session_manager;
    FakeGroupRepository group_repo;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::GroupService service(session_manager, group_repo, message_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");

    chat::CreateGroupRequest req;
    req.name = "study";
    req.member_ids = {2, 3};
    const chat::CreateGroupResult result = service.createGroup(100, req);

    assert(result.code == chat::ErrorCode::OK);
    assert(result.group.has_value());
    assert(result.group->owner_id == 1);
    assert(!result.group->id.empty());
    assert(result.member_ids.size() == 3);
    assert(group_repo.create_calls == 1);
    assert(group_repo.members[result.group->id].size() == 3);
    assert(group_repo.members[result.group->id][0].role == "owner");
}

void TestOwnerCanAddGroupMember() {
    FakeSessionManager session_manager;
    FakeGroupRepository group_repo;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    AddGroup(&group_repo, "grp_1", 1, {1, 2});
    chat::GroupService service(session_manager, group_repo, message_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");

    chat::AddGroupMemberRequest req;
    req.group_id = "grp_1";
    req.user_id = 3;
    const chat::AddGroupMemberResult result = service.addGroupMember(100, req);

    assert(result.code == chat::ErrorCode::OK);
    assert(result.member.has_value());
    assert(result.member->user_id == 3);
    assert(result.member->role == "member");
    assert(group_repo.add_member_calls == 1);
    assert(group_repo.members["grp_1"].size() == 3);
}

void TestNonAdminCannotAddGroupMember() {
    FakeSessionManager session_manager;
    FakeGroupRepository group_repo;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    AddGroup(&group_repo, "grp_1", 1, {1, 2});
    chat::GroupService service(session_manager, group_repo, message_repo, user_repo);
    BindUser(&session_manager, 200, 2, "user2");

    chat::AddGroupMemberRequest req;
    req.group_id = "grp_1";
    req.user_id = 3;
    const chat::AddGroupMemberResult result = service.addGroupMember(200, req);

    assert(result.code == chat::ErrorCode::PERMISSION_DENIED);
    assert(group_repo.add_member_calls == 0);
}

void TestSendGroupMessageFansOutToMembersExceptSender() {
    FakeSessionManager session_manager;
    FakeGroupRepository group_repo;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    AddGroup(&group_repo, "grp_1", 1, {1, 2, 3});
    chat::GroupService service(session_manager, group_repo, message_repo, user_repo);
    BindUser(&session_manager, 100, 1, "user1");
    BindUser(&session_manager, 200, 2, "user2");

    chat::SendGroupMessageRequest req;
    req.group_id = "grp_1";
    req.client_msg_id = "group_msg_1";
    req.content = "hello group";
    const chat::SendGroupMessageResult result = service.sendGroupMessage(100, req);

    assert(result.code == chat::ErrorCode::OK);
    assert(result.group_id == "grp_1");
    assert(result.conversation_id == "gconv_grp_1");
    assert(result.messages.size() == 2);
    assert(result.messages[0].to_user_id == 2);
    assert(result.messages[0].target_conn_id == 200);
    assert(result.messages[0].sequence == 1);
    assert(result.messages[1].to_user_id == 3);
    assert(result.messages[1].target_conn_id == 0);
    assert(result.messages[1].sequence == 2);
    assert(message_repo.created_messages.size() == 2);
}

void TestNonMemberCannotSendGroupMessage() {
    FakeSessionManager session_manager;
    FakeGroupRepository group_repo;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    AddGroup(&group_repo, "grp_1", 1, {1, 2});
    chat::GroupService service(session_manager, group_repo, message_repo, user_repo);
    BindUser(&session_manager, 300, 3, "user3");

    chat::SendGroupMessageRequest req;
    req.group_id = "grp_1";
    req.client_msg_id = "group_msg_1";
    req.content = "hello group";
    const chat::SendGroupMessageResult result = service.sendGroupMessage(300, req);

    assert(result.code == chat::ErrorCode::PERMISSION_DENIED);
    assert(message_repo.created_messages.empty());
}

}  // namespace

int main() {
    TestCreateGroupAddsOwnerAndMembers();
    TestOwnerCanAddGroupMember();
    TestNonAdminCannotAddGroupMember();
    TestSendGroupMessageFansOutToMembersExceptSender();
    TestNonMemberCannotSendGroupMessage();
    std::cout << "[PASS] group service tests passed\n";
    return 0;
}
