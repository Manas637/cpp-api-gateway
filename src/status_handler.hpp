#pragma once

#include <sstream>
#include <string>

#include "load_balancer.hpp"
#include "metrics.hpp"

class StatusHandler
{
public:
    static std::string build_json(
        const LoadBalancer &load_balancer,
        const Metrics &metrics,
        bool rate_limiting_enabled)
    {
        const auto &backends = load_balancer.backends();

        std::size_t healthy_backends = 0;

        for (const auto &backend : backends)
        {
            if (backend.healthy &&
                backend.circuit_breaker.state() !=
                    CircuitBreaker::State::OPEN)
            {
                ++healthy_backends;
            }
        }

        std::string gateway_status;

        if (healthy_backends == 0)
        {
            gateway_status = "unhealthy";
        }
        else if (healthy_backends < backends.size())
        {
            gateway_status = "degraded";
        }
        else
        {
            gateway_status = "healthy";
        }

        std::ostringstream json;

        json << "{";
        json << "\"status\":\"" << gateway_status << "\",";
        json << "\"active_connections\":"
             << metrics.active_connections()
             << ",";

        json << "\"rate_limiting\":{";
        json << "\"enabled\":"
             << (rate_limiting_enabled ? "true" : "false");
        json << "},";

        json << "\"backends\":[";

        for (std::size_t i = 0; i < backends.size(); ++i)
        {
            const auto &backend = backends[i];

            if (i > 0)
            {
                json << ",";
            }

            json << "{";

            json << "\"host\":\""
                 << backend.host
                 << "\",";

            json << "\"port\":"
                 << backend.port
                 << ",";

            json << "\"healthy\":"
                 << (backend.healthy ? "true" : "false")
                 << ",";

            json << "\"circuit\":\""
                 << circuit_state_to_string(
                        backend.circuit_breaker.state())
                 << "\",";

            json << "\"failure_count\":"
                 << backend.circuit_breaker.failure_count();

            json << "}";
        }

        json << "]";
        json << "}";

        return json.str();
    }

private:
    static const char *circuit_state_to_string(
        CircuitBreaker::State state)
    {
        switch (state)
        {
        case CircuitBreaker::State::CLOSED:
            return "CLOSED";

        case CircuitBreaker::State::OPEN:
            return "OPEN";

        case CircuitBreaker::State::HALF_OPEN:
            return "HALF_OPEN";
        }

        return "UNKNOWN";
    }
};