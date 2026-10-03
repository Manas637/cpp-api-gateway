#include "metrics.hpp"

// ============================================================
// HTTP request/response metrics
// ============================================================

void Metrics::record_request()
{
    requests_total_.fetch_add(1, std::memory_order_relaxed);
}

void Metrics::record_response(unsigned int status_code)
{
    if (status_code >= 200 && status_code < 300)
    {
        responses_2xx_total_.fetch_add(
            1,
            std::memory_order_relaxed);
    }
    else if (status_code >= 400 && status_code < 500)
    {
        responses_4xx_total_.fetch_add(
            1,
            std::memory_order_relaxed);
    }
    else if (status_code >= 500 && status_code < 600)
    {
        responses_5xx_total_.fetch_add(
            1,
            std::memory_order_relaxed);
    }
}

// ============================================================
// Backend metrics
// ============================================================

void Metrics::record_backend_request()
{
    backend_requests_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

void Metrics::record_backend_success()
{
    backend_successes_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

void Metrics::record_backend_failure()
{
    backend_failures_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

void Metrics::record_failover()
{
    failovers_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

// ============================================================
// Rate limiter metrics
// ============================================================

void Metrics::record_rate_limit_allowed()
{
    rate_limit_allowed_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

void Metrics::record_rate_limit_rejected()
{
    rate_limit_rejected_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

// ============================================================
// Circuit breaker metrics
// ============================================================

void Metrics::record_circuit_open()
{
    circuit_opens_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

void Metrics::record_circuit_half_open()
{
    circuit_half_opens_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

void Metrics::record_circuit_recovery()
{
    circuit_recoveries_total_.fetch_add(
        1,
        std::memory_order_relaxed);
}

// ============================================================
// Connection metrics
// ============================================================

void Metrics::connection_opened()
{
    active_connections_.fetch_add(
        1,
        std::memory_order_relaxed);
}

void Metrics::connection_closed()
{
    active_connections_.fetch_sub(
        1,
        std::memory_order_relaxed);
}

// ============================================================
// Accessors
// ============================================================

std::uint64_t Metrics::requests_total() const
{
    return requests_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::responses_2xx_total() const
{
    return responses_2xx_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::responses_4xx_total() const
{
    return responses_4xx_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::responses_5xx_total() const
{
    return responses_5xx_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::backend_requests_total() const
{
    return backend_requests_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::backend_successes_total() const
{
    return backend_successes_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::backend_failures_total() const
{
    return backend_failures_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::failovers_total() const
{
    return failovers_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::rate_limit_allowed_total() const
{
    return rate_limit_allowed_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::rate_limit_rejected_total() const
{
    return rate_limit_rejected_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::circuit_opens_total() const
{
    return circuit_opens_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::circuit_half_opens_total() const
{
    return circuit_half_opens_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::circuit_recoveries_total() const
{
    return circuit_recoveries_total_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::active_connections() const
{
    return active_connections_.load(
        std::memory_order_relaxed);
}