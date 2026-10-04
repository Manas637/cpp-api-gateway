#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <array>

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

    void record_request_duration(std::uint64_t duration_us);

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

    std::string to_prometheus() const;

    std::uint64_t request_duration_count() const;
    std::uint64_t request_duration_sum_us() const;

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

    static constexpr std::array<std::uint64_t, 12>
        request_duration_buckets_us_{
            1'000,     // 1 ms
            5'000,     // 5 ms
            10'000,    // 10 ms
            25'000,    // 25 ms
            50'000,    // 50 ms
            100'000,   // 100 ms
            250'000,   // 250 ms
            500'000,   // 500 ms
            1'000'000, // 1 s
            2'500'000, // 2.5 s
            5'000'000, // 5 s
            10'000'000 // 10 s
    };

    std::array<std::atomic<std::uint64_t>, 12>
        request_duration_bucket_counts_{};

    std::atomic<std::uint64_t>
        request_duration_count_{0};

    std::atomic<std::uint64_t>
        request_duration_sum_us_{0};
};