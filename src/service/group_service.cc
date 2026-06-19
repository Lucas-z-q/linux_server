#include "service/group_service.h"

#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include "common/validator.h"

namespace chat {
namespace {

constexpr std::size_t kMaxInitialGroupMembers = 50;

ErrorCode MapRepositoryError(RepositoryStatus status) {
    switch (status) {
        case RepositoryStatus::kDuplicate:
            return ErrorCode::GROUP_MEMBER_ALREADY_EXISTS;
        case RepositoryStatus::kInsertFailed:
            return ErrorCode::DB_INSERT_FAILED;
        case RepositoryStatus::kQueryFailed:
        case RepositoryStatus::kConnectionUnavailable:
        case RepositoryStatus::kBorrowTimeout:
        case RepositoryStatus::kNotFound:
            return ErrorCode::DB_QUERY_FAILED;
        default:
            return ErrorCode::INTERNAL_ERROR;
    }
}

std::optional<ConnectionSession> GetAuthenticatedSession(ISessionManager& session_manager, ConnectionId conn_id) {
    const std::optional<ConnectionSession> session = session_manager.GetSession(conn_id);
    if (!session.has_value() || !session->authenticated) {
        return std::nullopt;
    }
    return session;
}

bool RoleCanManageMembers(const std::string& role) { return role == "owner" || role == "admin"; }

std::optional<GroupMemberRecord> FindMember(const std::vector<GroupMemberRecord>& members, UserId user_id) {
    const auto it = std::find_if(members.begin(), members.end(),
                                 [user_id](const GroupMemberRecord& member) { return member.user_id == user_id; });
    if (it == members.end()) {
        return std::nullopt;
    }
    return *it;
}

std::string RandomHex(std::size_t byte_count) {
    std::string bytes(byte_count, '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count <= 0) {
            return "";
        }
        offset += static_cast<std::size_t>(count);
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

std::string BuildGroupId() {
    const std::string random = RandomHex(12);
    return random.empty() ? "" : "grp_" + random;
}

std::string BuildGroupConversationId(const std::string& group_id) { return "gconv_" + group_id; }

std::string BuildGroupMessageId(Timestamp now, UserId from_user_id, UserId to_user_id) {
    static std::atomic<uint64_t> group_msg_seq{0};
    static thread_local std::mt19937_64 rng(std::random_device{}());

    const uint64_t random_part = rng();
    const uint64_t mixed_part = (static_cast<uint64_t>(now) << 32) ^ (static_cast<uint64_t>(getpid()) << 16) ^
                                static_cast<uint64_t>(from_user_id) ^ (static_cast<uint64_t>(to_user_id) << 8) ^
                                (++group_msg_seq);

    std::ostringstream oss;
    oss << "gmsg_" << std::hex << std::setw(16) << std::setfill('0') << mixed_part << std::setw(16) << std::setfill('0')
        << random_part;
    return oss.str();
}

std::string BuildRecipientClientMessageId(const std::string& group_id, const std::string& client_msg_id,
                                          UserId to_user_id) {
    const std::string raw = group_id + ":" + client_msg_id + ":" + std::to_string(to_user_id);
    std::ostringstream out;
    out << "gcm_" << std::hex << std::hash<std::string>{}(raw);
    return out.str();
}

bool SameStoredGroupMessage(const MessageRecord& expected, const MessageRecord& actual) {
    return expected.conversation_id == actual.conversation_id && expected.client_msg_id == actual.client_msg_id &&
           expected.from_user_id == actual.from_user_id && expected.to_user_id == actual.to_user_id &&
           expected.content == actual.content;
}

std::optional<std::string> ValidateGroupId(const std::string& group_id) {
    const ValidationResult validation = Validator::ConversationId(group_id);
    return validation.ok() ? std::nullopt : std::optional<std::string>(validation.message);
}

bool UserExists(IUserRepository& user_repository, UserId user_id, RepositoryStatus* status) {
    const FindUserResult user = user_repository.findById(user_id);
    *status = user.status;
    return user.status == RepositoryStatus::kOk && user.user.has_value();
}

}  // namespace

GroupService::GroupService(ISessionManager& session_manager, IGroupRepository& group_repository,
                           IMessageRepository& message_repository, IUserRepository& user_repository)
    : session_manager_(session_manager),
      group_repository_(group_repository),
      message_repository_(message_repository),
      user_repository_(user_repository) {}

CreateGroupResult GroupService::createGroup(ConnectionId conn_id, const CreateGroupRequest& req) {
    const std::optional<ConnectionSession> session = GetAuthenticatedSession(session_manager_, conn_id);
    if (!session) {
        return {.code = ErrorCode::NOT_LOGGED_IN, .message = "User not logged in"};
    }
    const ValidationResult name_validation = Validator::Nickname(req.name);
    if (!name_validation.ok()) {
        return {.code = ErrorCode::INVALID_PARAM, .message = name_validation.message};
    }
    if (req.member_ids.size() > kMaxInitialGroupMembers) {
        return {.code = ErrorCode::INVALID_PARAM, .message = "too many initial group members"};
    }

    std::vector<UserId> unique_members = {session->user_id};
    std::unordered_set<UserId> seen = {session->user_id};
    for (const UserId member_id : req.member_ids) {
        if (member_id <= 0) {
            return {.code = ErrorCode::INVALID_PARAM, .message = "member_id must be greater than 0"};
        }
        if (seen.insert(member_id).second) {
            unique_members.push_back(member_id);
        }
    }
    for (const UserId member_id : unique_members) {
        RepositoryStatus user_status = RepositoryStatus::kNotFound;
        if (!UserExists(user_repository_, member_id, &user_status)) {
            return {.code = user_status == RepositoryStatus::kNotFound ? ErrorCode::USER_NOT_FOUND
                                                                       : MapRepositoryError(user_status),
                    .message = "group member not found"};
        }
    }

    GroupRecord group;
    group.id = BuildGroupId();
    if (group.id.empty()) {
        return {.code = ErrorCode::INTERNAL_ERROR, .message = "generate group id failed"};
    }
    group.name = req.name;
    group.owner_id = session->user_id;
    group.conversation_id = BuildGroupConversationId(group.id);

    std::vector<GroupMemberRecord> members;
    members.reserve(unique_members.size());
    for (const UserId member_id : unique_members) {
        GroupMemberRecord member;
        member.group_id = group.id;
        member.user_id = member_id;
        member.role = member_id == session->user_id ? "owner" : "member";
        members.push_back(member);
    }

    const GroupRepositoryCreateResult created = group_repository_.createGroup(group, members);
    if (created.status != RepositoryStatus::kOk || !created.group) {
        return {.code = MapRepositoryError(created.status), .message = "create group failed"};
    }
    return {.code = ErrorCode::OK, .message = "group created", .group = created.group, .member_ids = unique_members};
}

AddGroupMemberResult GroupService::addGroupMember(ConnectionId conn_id, const AddGroupMemberRequest& req) {
    const std::optional<ConnectionSession> session = GetAuthenticatedSession(session_manager_, conn_id);
    if (!session) {
        return {.code = ErrorCode::NOT_LOGGED_IN, .message = "User not logged in"};
    }
    if (const auto validation_error = ValidateGroupId(req.group_id)) {
        return {.code = ErrorCode::INVALID_PARAM, .message = *validation_error};
    }
    if (req.user_id <= 0) {
        return {.code = ErrorCode::INVALID_PARAM, .message = "user_id must be greater than 0"};
    }

    RepositoryStatus user_status = RepositoryStatus::kNotFound;
    if (!UserExists(user_repository_, req.user_id, &user_status)) {
        return {.code = user_status == RepositoryStatus::kNotFound ? ErrorCode::USER_NOT_FOUND
                                                                   : MapRepositoryError(user_status),
                .message = "target user not found"};
    }

    const GroupRepositoryFindResult group = group_repository_.findGroup(req.group_id);
    if (group.status == RepositoryStatus::kNotFound) {
        return {.code = ErrorCode::GROUP_NOT_FOUND, .message = "group not found"};
    }
    if (group.status != RepositoryStatus::kOk || !group.group) {
        return {.code = MapRepositoryError(group.status), .message = "query group failed"};
    }

    const GroupRepositoryListMembersResult members = group_repository_.listMembers(req.group_id);
    if (members.status != RepositoryStatus::kOk) {
        return {.code = MapRepositoryError(members.status), .message = "list group members failed"};
    }
    const std::optional<GroupMemberRecord> actor = FindMember(members.members, session->user_id);
    if (!actor || !RoleCanManageMembers(actor->role)) {
        return {.code = ErrorCode::PERMISSION_DENIED, .message = "permission denied"};
    }
    if (FindMember(members.members, req.user_id)) {
        return {.code = ErrorCode::GROUP_MEMBER_ALREADY_EXISTS, .message = "group member already exists"};
    }

    GroupMemberRecord member;
    member.group_id = req.group_id;
    member.user_id = req.user_id;
    member.role = "member";
    const GroupRepositoryAddMemberResult added = group_repository_.addMember(member);
    if (added.status == RepositoryStatus::kDuplicate) {
        return {.code = ErrorCode::GROUP_MEMBER_ALREADY_EXISTS, .message = "group member already exists"};
    }
    if (added.status != RepositoryStatus::kOk || !added.member) {
        return {.code = MapRepositoryError(added.status), .message = "add group member failed"};
    }
    return {.code = ErrorCode::OK, .message = "group member added", .member = added.member};
}

SendGroupMessageResult GroupService::sendGroupMessage(ConnectionId conn_id, const SendGroupMessageRequest& req) {
    const std::optional<ConnectionSession> session = GetAuthenticatedSession(session_manager_, conn_id);
    if (!session) {
        return {.code = ErrorCode::NOT_LOGGED_IN, .message = "User not logged in"};
    }
    if (const auto validation_error = ValidateGroupId(req.group_id)) {
        return {.code = ErrorCode::INVALID_PARAM, .message = *validation_error};
    }
    const ValidationResult client_id_validation = Validator::ClientMessageId(req.client_msg_id);
    if (!client_id_validation.ok()) {
        return {.code = ErrorCode::INVALID_PARAM, .message = client_id_validation.message};
    }
    const ValidationResult content_validation = Validator::MessageContent(req.content);
    if (!content_validation.ok()) {
        return {.code = req.content.size() > Validator::kMessageMaxBytes ? ErrorCode::MESSAGE_TOO_LONG
                                                                         : ErrorCode::INVALID_PARAM,
                .message = content_validation.message};
    }

    const GroupRepositoryFindResult group = group_repository_.findGroup(req.group_id);
    if (group.status == RepositoryStatus::kNotFound) {
        return {.code = ErrorCode::GROUP_NOT_FOUND, .message = "group not found"};
    }
    if (group.status != RepositoryStatus::kOk || !group.group) {
        return {.code = MapRepositoryError(group.status), .message = "query group failed"};
    }

    const GroupRepositoryListMembersResult members = group_repository_.listMembers(req.group_id);
    if (members.status != RepositoryStatus::kOk) {
        return {.code = MapRepositoryError(members.status), .message = "list group members failed"};
    }
    if (!FindMember(members.members, session->user_id)) {
        return {.code = ErrorCode::PERMISSION_DENIED, .message = "permission denied"};
    }

    const Timestamp now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    SendGroupMessageResult result;
    result.code = ErrorCode::OK;
    result.message = "Success";
    result.group_id = req.group_id;
    result.conversation_id = group.group->conversation_id;
    result.from_user_id = session->user_id;
    result.from_username = session->username;
    result.content = req.content;
    result.server_time = now;

    for (const GroupMemberRecord& member : members.members) {
        if (member.user_id == session->user_id) {
            continue;
        }

        MessageRecord pending;
        pending.id = BuildGroupMessageId(now, session->user_id, member.user_id);
        pending.conversation_id = group.group->conversation_id;
        pending.client_msg_id = BuildRecipientClientMessageId(req.group_id, req.client_msg_id, member.user_id);
        pending.from_user_id = session->user_id;
        pending.to_user_id = member.user_id;
        pending.content = req.content;
        pending.status = MessageStatus::kStored;
        pending.created_at = now;

        const CreateMessageResult created = message_repository_.createMessage(pending);
        if (created.status != RepositoryStatus::kOk || !created.message) {
            return {.code = MapRepositoryError(created.status), .message = "store group message failed"};
        }
        if (!SameStoredGroupMessage(pending, *created.message)) {
            return {.code = ErrorCode::IDEMPOTENCY_CONFLICT,
                    .message = "client_msg_id conflicts with existing group message"};
        }

        GroupMessageDelivery delivery;
        delivery.message_id = created.message->id;
        delivery.group_id = req.group_id;
        delivery.conversation_id = created.message->conversation_id;
        delivery.sequence = created.message->sequence;
        delivery.to_user_id = created.message->to_user_id;
        delivery.target_conn_id = session_manager_.GetConnectionId(created.message->to_user_id).value_or(0);
        delivery.status = ToProtocolMessageStatus(created.message->status);
        delivery.created_at = created.message->created_at;
        result.messages.push_back(delivery);
    }
    return result;
}

}  // namespace chat
