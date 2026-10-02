#pragma once

#include <string>

struct Backend
{
    std::string host;
    unsigned short port;

    bool healthy = true;

    Backend(
        std::string host,
        unsigned short port)
        : host(std::move(host)),
          port(port) {}
};