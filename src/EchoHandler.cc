#include "../include/EchoHandler.h"

/**
 * @file EchoHandler.cc
 * @brief Implements echo message handling logic.
 */

std::string EchoHandler::handle(const std::string& request,
                                chat::ConnectionId conn_id) {
    (void)conn_id;
    return request;
}
