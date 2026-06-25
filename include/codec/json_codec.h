#ifndef LINUX_SERVER_INCLUDE_CODEC_JSON_CODEC_H_
#define LINUX_SERVER_INCLUDE_CODEC_JSON_CODEC_H_

#include <string>

#include "common/message.h"
#include "common/response.h"
#include "packet_codec.h"
#include "protocol/auth_messages.h"
#include "protocol/chat_messages.h"
#include "protocol/friend_messages.h"
#include "protocol/group_messages.h"

// 本文件声明 JSON 编解码器。
// 该类负责在原始字符串与协议对象之间转换，不承担业务校验职责。

namespace chat {

// 负责处理协议对象与 JSON 字符串之间的编解码转换。
class JsonCodec {
   public:
    // 解析一段原始 JSON 字符串到通用消息对象。
    // 解析失败时返回 false，并通过 err 输出原因。
    bool decodeMessage(const std::string &raw, Message &msg, std::string &err) const;

    // 将通用响应对象编码为 JSON 字符串。
    std::string encodeResponse(const Response &resp) const;

    // 从通用消息中提取注册请求结构。
    bool parseRegisterRequest(const Message &msg, RegisterRequest &req, std::string &err) const;

    // 从通用消息中提取登录请求结构。
    bool parseLoginRequest(const Message &msg, LoginRequest &req, std::string &err) const;

    // 将注册响应业务数据写回通用响应对象。
    void fillRegisterResponse(Response &resp, const RegisterResponseData &data) const;

    // 将登录响应业务数据写回通用响应对象。
    void fillLoginResponse(Response &resp, const LoginResponseData &data) const;

    // 将心跳响应业务数据写回通用响应对象。
    void fillHeartbeatResponse(Response &resp, const HeartbeatResponseData &data) const;

    // 从通用消息中提取发送消息请求结构。
    bool parseSendMessageRequest(const Message &msg, SendMessageRequest &req, std::string &err) const;

    // 将发送消息确认数据写回通用响应对象。
    void fillSendMessageAck(Response &resp, const SendMessageAckData &data) const;

    // 将消息推送数据写回通用响应对象。
    void fillMessagePush(Response &resp, const MessagePushData &data) const;

    // 从通用消息中提取拉取离线消息请求结构。
    bool parsePullOfflineMessagesRequest(const Message &msg, PullOfflineMessagesRequest &req, std::string &err) const;

    // 将拉取离线消息响应业务数据写回通用响应对象。
    void fillPullOfflineMessagesResponse(Response &resp, const PullOfflineMessagesResponseData &data) const;

    bool parseMessageAckRequest(const Message &msg, MessageAckRequest &req, std::string &err) const;

    bool parseMarkMessageReadRequest(const Message &msg, MarkMessageReadRequest &req, std::string &err) const;

    void fillMessageStateUpdateResponse(Response &resp, const MessageStateUpdateResponseData &data) const;

    // 从通用消息中提取添加好友请求结构。
    bool parseAddFriendRequest(const Message &msg, AddFriendRequest &req, std::string &err) const;

    // 从通用消息中提取同意好友请求结构。
    bool parseAcceptFriendRequest(const Message &msg, AcceptFriendRequest &req, std::string &err) const;

    // 从通用消息中提取删除好友请求结构。
    bool parseDeleteFriendRequest(const Message &msg, DeleteFriendRequest &req, std::string &err) const;

    // 将好友关系操作响应业务数据写回通用响应对象。
    void fillFriendshipActionResponse(Response &resp, const FriendshipActionResponseData &data) const;

    // 将删除好友响应业务数据写回通用响应对象。
    void fillDeleteFriendResponse(Response &resp, const DeleteFriendResponseData &data) const;

    // 将好友列表响应业务数据写回通用响应对象。
    void fillListFriendsResponse(Response &resp, const ListFriendsResponseData &data) const;

    bool parseCreateGroupRequest(const Message &msg, CreateGroupRequest &req, std::string &err) const;

    bool parseAddGroupMemberRequest(const Message &msg, AddGroupMemberRequest &req, std::string &err) const;

    bool parseSendGroupMessageRequest(const Message &msg, SendGroupMessageRequest &req, std::string &err) const;

    void fillCreateGroupResponse(Response &resp, const CreateGroupResponseData &data) const;

    void fillAddGroupMemberResponse(Response &resp, const AddGroupMemberResponseData &data) const;

    void fillSendGroupMessageResponse(Response &resp, const SendGroupMessageResponseData &data) const;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CODEC_JSON_CODEC_H_
