#include "codec/json_codec.h"

#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

namespace chat {

namespace {

bool ExtractRequiredString(const nlohmann::json& data, const char* key,
                           std::string& value, std::string& err) {
  if (!data.contains(key) || !data.at(key).is_string()) {
    err = std::string("missing or invalid field: ") + key;
    return false;
  }
  value = data.at(key).get<std::string>();
  return true;
}

}  // namespace

bool JsonCodec::decodeMessage(const std::string& raw, Message& msg,
                              std::string& err) const {
  try {
    const auto root = nlohmann::json::parse(raw);
    if (!root.is_object()) {
      err = "root json must be object";
      return false;
    }

    if (!root.contains("msg_type") || !root.at("msg_type").is_string()) {
      err = "missing or invalid field: msg_type";
      return false;
    }
    msg.msg_type = root.at("msg_type").get<std::string>();

    if (!root.contains("seq") || !root.at("seq").is_number_unsigned()) {
      err = "missing or invalid field: seq";
      return false;
    }
    msg.seq = root.at("seq").get<SeqId>();

    if (root.contains("token")) {
      if (!root.at("token").is_string()) {
        err = "invalid field: token";
        return false;
      }
      msg.token = root.at("token").get<std::string>();
    } else {
      msg.token.clear();
    }

    if (!root.contains("data") || !root.at("data").is_object()) {
      err = "missing or invalid field: data";
      return false;
    }
    msg.data = root.at("data");
    return true;
  } catch (const std::exception& ex) {
    err = ex.what();
    return false;
  }
}

std::string JsonCodec::encodeResponse(const Response& resp) const {
  nlohmann::json root;
  root["msg_type"] = resp.msg_type;
  root["seq"] = resp.seq;
  root["code"] = static_cast<int>(resp.code);
  root["message"] = resp.message;
  root["data"] = resp.data;
  return root.dump();
}

bool JsonCodec::parseRegisterRequest(const Message& msg, RegisterRequest& req,
                                     std::string& err) const {
  if (!ExtractRequiredString(msg.data, "username", req.username, err) ||
      !ExtractRequiredString(msg.data, "password", req.password, err)) {
    return false;
  }

  if (msg.data.contains("nickname")) {
    if (!msg.data.at("nickname").is_string()) {
      err = "invalid field: nickname";
      return false;
    }
    req.nickname = msg.data.at("nickname").get<std::string>();
  } else {
    req.nickname.clear();
  }
  return true;
}

bool JsonCodec::parseLoginRequest(const Message& msg, LoginRequest& req,
                                  std::string& err) const {
  return ExtractRequiredString(msg.data, "username", req.username, err) &&
         ExtractRequiredString(msg.data, "password", req.password, err);
}

void JsonCodec::fillRegisterResponse(Response& resp,
                                     const RegisterResponseData& data) const {
  resp.data = nlohmann::json{{"user_id", data.user_id}};
}

void JsonCodec::fillLoginResponse(Response& resp,
                                  const LoginResponseData& data) const {
  resp.data = nlohmann::json{{"user_id", data.user_id},
                             {"nickname", data.nickname},
                             {"token", data.token}};
}

void JsonCodec::fillHeartbeatResponse(Response& resp,
                                      const HeartbeatResponseData& data) const {
  resp.data = nlohmann::json{{"server_time", data.server_time}};
}

}  // namespace chat
