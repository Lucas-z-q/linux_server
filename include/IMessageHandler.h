#pragma once

#include <string>

class IMessageHandler {
public:
    virtual ~IMessageHandler() = default;

    virtual std::string handle(const std::string& request) = 0;
};