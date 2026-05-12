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
