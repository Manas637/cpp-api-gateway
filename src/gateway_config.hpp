#pragma once

#include <chrono>
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

    std::chrono::milliseconds backend_connect_timeout{2000};
    std::chrono::milliseconds backend_response_timeout{5000};
    std::chrono::milliseconds request_timeout{10000};
};