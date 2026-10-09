#pragma once

#include <vector>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "backend.hpp"

enum class LoadBalancingStrategy
{
    ROUND_ROBIN,
    LEAST_CONNECTIONS
};

class LoadBalancer
{
private:
    std::vector<Backend> backends_;
    std::size_t current_index_ = 0;
    LoadBalancingStrategy strategy_ =
        LoadBalancingStrategy::ROUND_ROBIN;

    Backend &next_round_robin(
        CircuitBreaker::Transition *transition,
        const Backend *excluded_backend)
    {
        const std::size_t count = backends_.size();

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

    Backend &next_least_connections(
        CircuitBreaker::Transition *transition,
        const Backend *excluded_backend)
    {
        const std::size_t count = backends_.size();

        Backend *best = nullptr;
        std::size_t best_index = 0;
        std::size_t best_in_flight = 0;

        const CircuitBreaker::TimePoint now =
            CircuitBreaker::Clock::now();

        // Candidates are CLOSED circuits plus OPEN circuits whose
        // cooldown has elapsed. Readiness is checked without calling
        // allow_request(), so the single HALF_OPEN probe is only
        // consumed for the backend that is actually selected.
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::size_t index = (current_index_ + i) % count;

            Backend &backend = backends_[index];

            // Never select the backend that just failed during a retry.
            if (&backend == excluded_backend)
            {
                continue;
            }

            if (!backend.healthy)
            {
                continue;
            }

            const bool closed =
                backend.circuit_breaker.state() ==
                CircuitBreaker::State::CLOSED;

            const bool recoverable =
                backend.circuit_breaker.is_open_and_ready(now);

            if (!closed && !recoverable)
            {
                continue;
            }

            const std::size_t in_flight =
                backend.in_flight_requests.load(
                    std::memory_order_relaxed);

            if (best == nullptr || in_flight < best_in_flight)
            {
                best = &backend;
                best_index = index;
                best_in_flight = in_flight;
            }
        }

        if (best != nullptr)
        {
            // Ties keep the first candidate in round-robin order and
            // the rotation advances past it, so equal counts do not
            // starve later backends.
            current_index_ = (best_index + 1) % count;

            // Selecting a cooldown-elapsed OPEN backend consumes its
            // HALF_OPEN probe and reports the transition. CLOSED
            // circuits simply report NONE.
            CircuitBreaker::Transition selected_transition =
                CircuitBreaker::Transition::NONE;

            best->circuit_breaker.allow_request(
                CircuitBreaker::Clock::now(),
                &selected_transition);

            if (transition != nullptr)
            {
                *transition = selected_transition;
            }

            return *best;
        }

        // Every candidate is OPEN, HALF_OPEN, unhealthy, or excluded.
        // Probe in order so an OPEN circuit that is ready can move to
        // HALF_OPEN exactly once.
        return next_round_robin(transition, excluded_backend);
    }

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
        if (transition != nullptr)
        {
            *transition = CircuitBreaker::Transition::NONE;
        }

        if (strategy_ == LoadBalancingStrategy::LEAST_CONNECTIONS)
        {
            return next_least_connections(
                transition, excluded_backend);
        }

        return next_round_robin(transition, excluded_backend);
    }

    std::vector<Backend> &backends()
    {
        return backends_;
    }

    const std::vector<Backend> &backends() const
    {
        return backends_;
    }

    void set_strategy(LoadBalancingStrategy strategy)
    {
        strategy_ = strategy;
    }
};