
#pragma once

#include <atomic>
#include <string>
#include <utility>

#include "circuit_breaker.hpp"

struct Backend
{
    std::string host;
    unsigned short port;

    bool healthy = true;

    // Number of requests currently assigned to this backend.
    std::atomic<std::size_t> in_flight_requests{0};

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

    // Atomic members make the implicit copy/move operations unavailable.
    // Backends are stored in a vector, so define these explicitly.
    Backend(const Backend &other)
        : host(other.host),
          port(other.port),
          healthy(other.healthy),
          in_flight_requests(
              other.in_flight_requests.load(std::memory_order_relaxed)),
          circuit_breaker(other.circuit_breaker)
    {
    }

    Backend(Backend &&other) noexcept
        : host(std::move(other.host)),
          port(other.port),
          healthy(other.healthy),
          in_flight_requests(
              other.in_flight_requests.load(std::memory_order_relaxed)),
          circuit_breaker(std::move(other.circuit_breaker))
    {
    }

    Backend &operator=(const Backend &other)
    {
        if (this == &other)
            return *this;

        host = other.host;
        port = other.port;
        healthy = other.healthy;

        in_flight_requests.store(
            other.in_flight_requests.load(std::memory_order_relaxed),
            std::memory_order_relaxed);

        circuit_breaker = other.circuit_breaker;

        return *this;
    }

    Backend &operator=(Backend &&other)
    {
        if (this == &other)
            return *this;

        host = std::move(other.host);
        port = other.port;
        healthy = other.healthy;

        in_flight_requests.store(
            other.in_flight_requests.load(std::memory_order_relaxed),
            std::memory_order_relaxed);

        circuit_breaker = std::move(other.circuit_breaker);

        return *this;
    }
};
