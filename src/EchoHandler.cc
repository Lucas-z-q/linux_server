#include "../include/EchoHandler.h"

/**
 * @file EchoHandler.cc
 * @brief Implements echo message handling logic.
 */

HandleResult EchoHandler::handle(const std::string& request, chat::ConnectionId conn_id) {
    (void)conn_id;
    HandleResult result;
    result.response = request;
    return result;
}

bool EchoHandler::isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) {
    (void)conn_id;
    (void)user_id;
    return true;
}
