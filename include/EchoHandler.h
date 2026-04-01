#pragma once

#include <string>
#include "IMessageHandler.h"

class EchoHandler : public IMessageHandler {
public:
    std::string handle(const std::string& request) override;
};