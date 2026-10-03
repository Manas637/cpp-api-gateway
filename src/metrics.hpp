#pragma once

#include <atomic>
#include <cstdint>

class Metrics
{
public:
    // HTTP request/response metrics
    void record_request();

    void record_response(unsigned int status_code);

    // Backend metrics
    void record_backend_request();

    void record_backend_success();

    void record_backend_failure();

    void record_failover();

    // Rate limiter metrics
    void record_rate_limit_allowed();

    void record_rate_limit_rejected();

    // Circuit breaker metrics
    void record_circuit_open();

    void record_circuit_half_open();

    void record_circuit_recovery();

    // Connection metrics
    void connection_opened();

    void connection_closed();

    // Read-only accessors.
    std::uint64_t requests_total() const;

    std::uint64_t responses_2xx_total() const;

    std::uint64_t responses_4xx_total() const;

    std::uint64_t responses_5xx_total() const;

    std::uint64_t backend_requests_total() const;

    std::uint64_t backend_successes_total() const;

    std::uint64_t backend_failures_total() const;

    std::uint64_t failovers_total() const;

    std::uint64_t rate_limit_allowed_total() const;

    std::uint64_t rate_limit_rejected_total() const;

    std::uint64_t circuit_opens_total() const;

    std::uint64_t circuit_half_opens_total() const;

    std::uint64_t circuit_recoveries_total() const;

    std::uint64_t active_connections() const;

private:
    std::atomic<std::uint64_t> requests_total_{0};

    std::atomic<std::uint64_t> responses_2xx_total_{0};
    std::atomic<std::uint64_t> responses_4xx_total_{0};
    std::atomic<std::uint64_t> responses_5xx_total_{0};

    std::atomic<std::uint64_t> backend_requests_total_{0};
    std::atomic<std::uint64_t> backend_successes_total_{0};
    std::atomic<std::uint64_t> backend_failures_total_{0};
    std::atomic<std::uint64_t> failovers_total_{0};

    std::atomic<std::uint64_t> rate_limit_allowed_total_{0};
    std::atomic<std::uint64_t> rate_limit_rejected_total_{0};

    std::atomic<std::uint64_t> circuit_opens_total_{0};
    std::atomic<std::uint64_t> circuit_half_opens_total_{0};
    std::atomic<std::uint64_t> circuit_recoveries_total_{0};

    std::atomic<std::uint64_t> active_connections_{0};
};