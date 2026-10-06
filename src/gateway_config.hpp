#pragma once

#include <string>
#include <vector>

#include "backend.hpp"

struct GatewayConfig
{
    unsigned short port = 8080;

    std::vector<Backend> backends;

    bool rate_limiting_enabled = true;
    double rate_limit_capacity = 100000.0;
    double rate_limit_refill_rate = 100000.0;

    std::string redis_host = "127.0.0.1";
    std::string redis_port = "6379";
};