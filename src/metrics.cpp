#include <sstream>
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

void Metrics::record_request_timeout()
{
    request_timeouts_total_.fetch_add(
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

std::uint64_t Metrics::request_timeouts_total() const
{
    return request_timeouts_total_.load(
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

std::string Metrics::to_prometheus() const
{
    std::ostringstream output;

    output
        << "# HELP gateway_requests_total Total HTTP requests received by the gateway\n"
        << "# TYPE gateway_requests_total counter\n"
        << "gateway_requests_total "
        << requests_total()
        << '\n';

    output
        << "# HELP gateway_responses_2xx_total Total HTTP 2xx responses\n"
        << "# TYPE gateway_responses_2xx_total counter\n"
        << "gateway_responses_2xx_total "
        << responses_2xx_total()
        << '\n';

    output
        << "# HELP gateway_responses_4xx_total Total HTTP 4xx responses\n"
        << "# TYPE gateway_responses_4xx_total counter\n"
        << "gateway_responses_4xx_total "
        << responses_4xx_total()
        << '\n';

    output
        << "# HELP gateway_responses_5xx_total Total HTTP 5xx responses\n"
        << "# TYPE gateway_responses_5xx_total counter\n"
        << "gateway_responses_5xx_total "
        << responses_5xx_total()
        << '\n';

    output
        << "# HELP gateway_backend_requests_total Total requests sent to backends\n"
        << "# TYPE gateway_backend_requests_total counter\n"
        << "gateway_backend_requests_total "
        << backend_requests_total()
        << '\n';

    output
        << "# HELP gateway_backend_successes_total Total successful backend responses\n"
        << "# TYPE gateway_backend_successes_total counter\n"
        << "gateway_backend_successes_total "
        << backend_successes_total()
        << '\n';

    output
        << "# HELP gateway_backend_failures_total Total backend failures\n"
        << "# TYPE gateway_backend_failures_total counter\n"
        << "gateway_backend_failures_total "
        << backend_failures_total()
        << '\n';

    output
        << "# HELP gateway_failovers_total Total backend failovers\n"
        << "# TYPE gateway_failovers_total counter\n"
        << "gateway_failovers_total "
        << failovers_total()
        << '\n';

    output
        << "# HELP gateway_request_timeouts_total Total requests that exceeded the overall request timeout\n"
        << "# TYPE gateway_request_timeouts_total counter\n"
        << "gateway_request_timeouts_total "
        << request_timeouts_total()
        << '\n';

    output
        << "# HELP gateway_rate_limit_allowed_total Total requests allowed by the rate limiter\n"
        << "# TYPE gateway_rate_limit_allowed_total counter\n"
        << "gateway_rate_limit_allowed_total "
        << rate_limit_allowed_total()
        << '\n';

    output
        << "# HELP gateway_rate_limit_rejected_total Total requests rejected by the rate limiter\n"
        << "# TYPE gateway_rate_limit_rejected_total counter\n"
        << "gateway_rate_limit_rejected_total "
        << rate_limit_rejected_total()
        << '\n';

    output
        << "# HELP gateway_circuit_opens_total Total circuit breaker openings\n"
        << "# TYPE gateway_circuit_opens_total counter\n"
        << "gateway_circuit_opens_total "
        << circuit_opens_total()
        << '\n';

    output
        << "# HELP gateway_circuit_half_opens_total Total circuit breaker half-open transitions\n"
        << "# TYPE gateway_circuit_half_opens_total counter\n"
        << "gateway_circuit_half_opens_total "
        << circuit_half_opens_total()
        << '\n';

    output
        << "# HELP gateway_circuit_recoveries_total Total circuit breaker recoveries\n"
        << "# TYPE gateway_circuit_recoveries_total counter\n"
        << "gateway_circuit_recoveries_total "
        << circuit_recoveries_total()
        << '\n';

    output
        << "# HELP gateway_active_connections Current number of active client connections\n"
        << "# TYPE gateway_active_connections gauge\n"
        << "gateway_active_connections "
        << active_connections()
        << '\n';

    output
        << "# HELP gateway_request_duration_seconds "
           "HTTP request duration in seconds\n"
        << "# TYPE gateway_request_duration_seconds histogram\n";

    for (std::size_t i = 0;
         i < request_duration_buckets_us_.size();
         ++i)
    {
        const double seconds =
            static_cast<double>(
                request_duration_buckets_us_[i]) /
            1'000'000.0;

        output
            << "gateway_request_duration_seconds_bucket{le=\""
            << seconds
            << "\"} "
            << request_duration_bucket_counts_[i].load(
                   std::memory_order_relaxed)
            << '\n';
    }

    output
        << "gateway_request_duration_seconds_bucket{le=\"+Inf\"} "
        << request_duration_count()
        << '\n';

    output
        << "gateway_request_duration_seconds_sum "
        << static_cast<double>(request_duration_sum_us()) /
               1'000'000.0
        << '\n';

    output
        << "gateway_request_duration_seconds_count "
        << request_duration_count()
        << '\n';

    return output.str();
}

void Metrics::record_request_duration(std::uint64_t duration_us)
{
    request_duration_count_.fetch_add(
        1,
        std::memory_order_relaxed);

    request_duration_sum_us_.fetch_add(
        duration_us,
        std::memory_order_relaxed);

    for (std::size_t i = 0;
         i < request_duration_buckets_us_.size();
         ++i)
    {
        if (duration_us <= request_duration_buckets_us_[i])
        {
            request_duration_bucket_counts_[i].fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }
}

std::uint64_t Metrics::request_duration_count() const
{
    return request_duration_count_.load(
        std::memory_order_relaxed);
}

std::uint64_t Metrics::request_duration_sum_us() const
{
    return request_duration_sum_us_.load(
        std::memory_order_relaxed);
}