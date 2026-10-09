#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include "metrics.hpp"

int main()
{
    Metrics metrics;

    // ============================================================
    // TEST 1: All metrics start at zero
    // ============================================================

    assert(metrics.requests_total() == 0);

    assert(metrics.responses_2xx_total() == 0);
    assert(metrics.responses_4xx_total() == 0);
    assert(metrics.responses_5xx_total() == 0);

    assert(metrics.backend_requests_total() == 0);
    assert(metrics.backend_successes_total() == 0);
    assert(metrics.backend_failures_total() == 0);
    assert(metrics.failovers_total() == 0);

    assert(metrics.request_timeouts_total() == 0);

    assert(metrics.rate_limit_allowed_total() == 0);
    assert(metrics.rate_limit_rejected_total() == 0);

    assert(metrics.circuit_opens_total() == 0);
    assert(metrics.circuit_half_opens_total() == 0);
    assert(metrics.circuit_recoveries_total() == 0);

    assert(metrics.active_connections() == 0);

    std::cout
        << "[PASS] metrics initialize to zero\n";

    // ============================================================
    // TEST 2: Request counter
    // ============================================================

    metrics.record_request();
    metrics.record_request();
    metrics.record_request();

    assert(metrics.requests_total() == 3);

    std::cout
        << "[PASS] request counter\n";

    // ============================================================
    // TEST 3: HTTP response classification
    // ============================================================

    metrics.record_response(200);
    metrics.record_response(201);
    metrics.record_response(204);

    metrics.record_response(400);
    metrics.record_response(404);

    metrics.record_response(500);
    metrics.record_response(502);
    metrics.record_response(503);

    assert(metrics.responses_2xx_total() == 3);
    assert(metrics.responses_4xx_total() == 2);
    assert(metrics.responses_5xx_total() == 3);

    std::cout
        << "[PASS] HTTP response classification\n";

    // ============================================================
    // TEST 4: Backend metrics
    // ============================================================

    metrics.record_backend_request();
    metrics.record_backend_request();
    metrics.record_backend_request();

    metrics.record_backend_success();
    metrics.record_backend_success();

    metrics.record_backend_failure();

    metrics.record_failover();
    metrics.record_failover();

    assert(metrics.backend_requests_total() == 3);
    assert(metrics.backend_successes_total() == 2);
    assert(metrics.backend_failures_total() == 1);
    assert(metrics.failovers_total() == 2);

    std::cout
        << "[PASS] backend metrics\n";

    // ============================================================
    // TEST 5: Rate limiter metrics
    // ============================================================

    metrics.record_rate_limit_allowed();
    metrics.record_rate_limit_allowed();
    metrics.record_rate_limit_allowed();

    metrics.record_rate_limit_rejected();

    assert(
        metrics.rate_limit_allowed_total() == 3);

    assert(
        metrics.rate_limit_rejected_total() == 1);

    std::cout
        << "[PASS] rate limiter metrics\n";

    // ============================================================
    // TEST 6: Circuit breaker metrics
    // ============================================================

    metrics.record_circuit_open();
    metrics.record_circuit_open();

    metrics.record_circuit_half_open();

    metrics.record_circuit_recovery();

    assert(
        metrics.circuit_opens_total() == 2);

    assert(
        metrics.circuit_half_opens_total() == 1);

    assert(
        metrics.circuit_recoveries_total() == 1);

    std::cout
        << "[PASS] circuit breaker metrics\n";

    // ============================================================
    // TEST 7: Active connections
    // ============================================================

    metrics.connection_opened();
    metrics.connection_opened();
    metrics.connection_opened();

    assert(
        metrics.active_connections() == 3);

    metrics.connection_closed();

    assert(
        metrics.active_connections() == 2);

    metrics.connection_closed();

    assert(
        metrics.active_connections() == 1);

    metrics.connection_closed();

    assert(
        metrics.active_connections() == 0);

    std::cout
        << "[PASS] active connection gauge\n";

    // ============================================================
    // TEST 8: Concurrent request updates
    // ============================================================

    constexpr int thread_count = 8;
    constexpr int increments_per_thread = 10000;

    std::vector<std::thread> threads;

    threads.reserve(thread_count);

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back(
            [&metrics]()
            {
                for (int j = 0;
                     j < increments_per_thread;
                     ++j)
                {
                    metrics.record_request();
                }
            });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    const std::uint64_t expected_requests =
        3 +
        static_cast<std::uint64_t>(
            thread_count * increments_per_thread);

    assert(
        metrics.requests_total() ==
        expected_requests);

    std::cout
        << "[PASS] concurrent request updates\n";

    // ============================================================
    // TEST 9: Concurrent backend updates
    // ============================================================

    threads.clear();

    constexpr int backend_updates_per_thread = 5000;

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back(
            [&metrics]()
            {
                for (int j = 0;
                     j < backend_updates_per_thread;
                     ++j)
                {
                    metrics.record_backend_request();
                    metrics.record_backend_success();
                    metrics.record_backend_failure();
                    metrics.record_failover();
                }
            });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    const std::uint64_t expected_backend_updates =
        3 +
        static_cast<std::uint64_t>(
            thread_count *
            backend_updates_per_thread);

    const std::uint64_t expected_failovers =
        2 +
        static_cast<std::uint64_t>(
            thread_count *
            backend_updates_per_thread);

    assert(
        metrics.backend_requests_total() ==
        expected_backend_updates);

    assert(
        metrics.backend_successes_total() ==
        2 +
            static_cast<std::uint64_t>(
                thread_count *
                backend_updates_per_thread));

    assert(
        metrics.backend_failures_total() ==
        1 +
            static_cast<std::uint64_t>(
                thread_count *
                backend_updates_per_thread));

    assert(
        metrics.failovers_total() ==
        expected_failovers);

    std::cout
        << "[PASS] concurrent backend updates\n";

    // ============================================================
    // TEST 10: Request timeout counter
    // ============================================================

    const auto timeouts_before =
        metrics.request_timeouts_total();

    metrics.record_request_timeout();
    metrics.record_request_timeout();
    metrics.record_request_timeout();

    assert(
        metrics.request_timeouts_total() ==
        timeouts_before + 3);

    std::cout
        << "[PASS] request timeout counter\n";

    // ============================================================
    // Final
    // ============================================================

    std::cout
        << "\nAll metrics tests passed.\n";

    return 0;
}