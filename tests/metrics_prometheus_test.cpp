#include <cassert>
#include <iostream>
#include <string>

#include "metrics.hpp"

int main()
{
    Metrics metrics;

    // Populate representative values.
    metrics.record_request();
    metrics.record_request();

    metrics.record_response(200);
    metrics.record_response(404);
    metrics.record_response(503);

    metrics.record_backend_request();
    metrics.record_backend_request();

    metrics.record_backend_success();
    metrics.record_backend_failure();

    metrics.record_failover();

    metrics.record_rate_limit_allowed();
    metrics.record_rate_limit_rejected();

    metrics.record_circuit_open();
    metrics.record_circuit_half_open();
    metrics.record_circuit_recovery();

    metrics.connection_opened();

    // Request latency histogram.
    // 3ms, 8ms, and 30ms.
    metrics.record_request_duration(3'000);
    metrics.record_request_duration(8'000);
    metrics.record_request_duration(30'000);

    const std::string output =
        metrics.to_prometheus();

    // Check request latency histogram values.

    assert(
        output.find(
            "gateway_request_duration_seconds_bucket{le=\"0.001\"} 0\n") != std::string::npos);

    assert(
        output.find(
            "gateway_request_duration_seconds_bucket{le=\"0.005\"} 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_request_duration_seconds_bucket{le=\"0.01\"} 2\n") != std::string::npos);

    assert(
        output.find(
            "gateway_request_duration_seconds_bucket{le=\"0.025\"} 2\n") != std::string::npos);

    assert(
        output.find(
            "gateway_request_duration_seconds_bucket{le=\"0.05\"} 3\n") != std::string::npos);

    assert(
        output.find(
            "gateway_request_duration_seconds_bucket{le=\"+Inf\"} 3\n") != std::string::npos);

    // Check histogram count.
    assert(
        output.find(
            "gateway_request_duration_seconds_count 3\n") != std::string::npos);

    // Check histogram sum: 3ms + 8ms + 30ms = 41ms.
    assert(
        output.find(
            "gateway_request_duration_seconds_sum 0.041\n") != std::string::npos);

    // Check Prometheus histogram metadata.
    assert(
        output.find(
            "# TYPE gateway_request_duration_seconds histogram\n") != std::string::npos);

    std::cout
        << "[PASS] Request latency histogram rendering\n";

    // Check metric values.
    assert(
        output.find(
            "gateway_requests_total 2\n") != std::string::npos);

    assert(
        output.find(
            "gateway_responses_2xx_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_responses_4xx_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_responses_5xx_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_backend_requests_total 2\n") != std::string::npos);

    assert(
        output.find(
            "gateway_backend_successes_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_backend_failures_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_failovers_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_rate_limit_allowed_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_rate_limit_rejected_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_circuit_opens_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_circuit_half_opens_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_circuit_recoveries_total 1\n") != std::string::npos);

    assert(
        output.find(
            "gateway_active_connections 1\n") != std::string::npos);

    // Check Prometheus metadata.
    assert(
        output.find(
            "# TYPE gateway_requests_total counter\n") != std::string::npos);

    assert(
        output.find(
            "# TYPE gateway_active_connections gauge\n") != std::string::npos);

    std::cout
        << "[PASS] Prometheus metrics rendering\n";

    return 0;
}