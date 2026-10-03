#pragma once

#include <string>
#include <utility>

#include "circuit_breaker.hpp"

struct Backend
{
    std::string host;
    unsigned short port;

    bool healthy = true;

    CircuitBreaker circuit_breaker;

    Backend(
        std::string host,
        unsigned short port,
        std::size_t failure_threshold = 5,
        std::chrono::milliseconds open_duration = std::chrono::seconds(5))
        : host(std::move(host)),
          port(port),
          circuit_breaker(
              failure_threshold,
              open_duration)
    {
    }
};