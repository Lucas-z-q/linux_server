#include "protocol/protocol_helper.h"

#include <utility>

namespace chat::protocol {

std::string makeResponseType(const std::string& request_type) {
  return request_type + "_resp";
}

Response makeSuccessResponse(const Message& request) {
  Response response;
  response.msg_type = makeResponseType(request.msg_type);
  response.seq = request.seq;
  response.code = ErrorCode::OK;
  response.message = "ok";
  response.data = nlohmann::json::object();
  return response;
}

Response makeErrorResponse(const Message& request, ErrorCode code,
                           const std::string& message) {
  Response response;
  response.msg_type = makeResponseType(request.msg_type);
  response.seq = request.seq;
  response.code = code;
  response.message = message;
  response.data = nlohmann::json::object();
  return response;
}

bool isAuthMessage(const std::string& msg_type) {
  return msg_type == "register" || msg_type == "login" ||
         msg_type == "logout";
}

}  // namespace chat::protocol
