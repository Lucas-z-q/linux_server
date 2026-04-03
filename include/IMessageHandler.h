#pragma once

#include <string>

/**
 * @file IMessageHandler.h
 * @brief Defines the message handler interface used by the TCP server.
 */

/**
 * @brief Interface for processing a client request and generating a response.
 */
class IMessageHandler {
public:
    /**
     * @brief Virtual destructor for safe polymorphic use.
     */
    virtual ~IMessageHandler() = default;

    /**
     * @brief Handles one request payload and returns the response payload.
     * @param request Request data received from the client.
     * @return Response data to send back to the client.
     */
    virtual std::string handle(const std::string& request) = 0;
};
