#pragma once

#include <vector>

#include "backend.hpp"

struct GatewayConfig
{
    unsigned short port = 8080;

    std::vector<Backend> backends;

    double rate_limit_capacity = 100000.0;
    double rate_limit_refill_rate = 100000.0;
};