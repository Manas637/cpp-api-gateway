#pragma once

#include <vector>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "backend.hpp"

class LoadBalancer
{
private:
    std::vector<Backend> backends_;
    std::size_t current_index_ = 0;

public:
    LoadBalancer(
        std::vector<Backend> backends)
        : backends_(std::move(backends))
    {
    }

    Backend &next(
        CircuitBreaker::Transition *transition = nullptr,
        const Backend *excluded_backend = nullptr)
    {
        const std::size_t count = backends_.size();

        if (transition != nullptr)
        {
            *transition = CircuitBreaker::Transition::NONE;
        }

        for (std::size_t i = 0; i < count; ++i)
        {
            Backend &backend = backends_[current_index_];

            current_index_ = (current_index_ + 1) % count;

            // Never select the backend that just failed during a retry.
            if (&backend == excluded_backend)
            {
                continue;
            }

            if (!backend.healthy)
            {
                continue;
            }

            CircuitBreaker::Transition current_transition =
                CircuitBreaker::Transition::NONE;

            if (backend.circuit_breaker.allow_request(
                    CircuitBreaker::Clock::now(),
                    &current_transition))
            {
                if (transition != nullptr)
                {
                    *transition = current_transition;
                }

                return backend;
            }
        }

        throw std::runtime_error("No healthy backend available");
    }

    std::vector<Backend> &backends()
    {
        return backends_;
    }

    const std::vector<Backend> &backends() const
    {
        return backends_;
    }
};