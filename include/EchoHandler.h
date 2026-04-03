#pragma once

#include <string>
#include "IMessageHandler.h"

/**
 * @file EchoHandler.h
 * @brief Declares an echo message handler implementation.
 */

/**
 * @brief Message handler that returns the original request unchanged.
 */
class EchoHandler : public IMessageHandler {
public:
    /**
     * @brief Returns the request payload as the response payload.
     * @param request Request data received from the client.
     * @return Same content as @p request.
     */
    std::string handle(const std::string& request) override;
};
