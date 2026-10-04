#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include "backend.hpp"
#include "gateway_config.hpp"
#include "in_memory_rate_limit_store.hpp"
#include "load_balancer.hpp"
#include "rate_limiter.hpp"
#include "server.hpp"
#include "metrics.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

// -----------------------------------------------------------------------------
// Sends one HTTP request to the gateway and waits for the complete response.
// -----------------------------------------------------------------------------

std::string send_request(
    unsigned short gateway_port,
    const std::string &target,
    unsigned short expected_status)
{
    asio::io_context io_context;

    tcp::resolver resolver(io_context);
    tcp::socket socket(io_context);

    auto endpoints = resolver.resolve(
        "127.0.0.1",
        std::to_string(gateway_port));

    asio::connect(socket, endpoints);

    http::request<http::string_body> request{
        http::verb::get,
        target,
        11};

    request.set(http::field::host, "localhost");
    request.set(http::field::connection, "close");

    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;

    http::read(socket, buffer, response);

    assert(response.result_int() == expected_status);

    return response.body();
}

// -----------------------------------------------------------------------------
// Wait for an asynchronously recorded request-latency sample.
//
// The gateway records request latency in the async_write completion handler.
// The client may finish reading the HTTP response before that handler has
// updated the Metrics object, so the test must synchronize with the metric.
// -----------------------------------------------------------------------------

bool wait_for_request_duration_count(
    const std::shared_ptr<Metrics> &metrics,
    std::uint64_t expected_count,
    std::chrono::milliseconds timeout =
        std::chrono::milliseconds(1000))
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (metrics->request_duration_count() >= expected_count)
            return true;

        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }

    return metrics->request_duration_count() >= expected_count;
}

int main()
{
    constexpr unsigned short gateway_port = 18082;
    constexpr unsigned short backend_port = 19004;

    asio::io_context io_context;

    // -------------------------------------------------------------------------
    // Backend
    // -------------------------------------------------------------------------

    auto acceptor = std::make_shared<tcp::acceptor>(
        io_context,
        tcp::endpoint(tcp::v4(), backend_port));

    std::function<void()> do_accept;

    do_accept = [&]()
    {
        if (!acceptor->is_open())
            return;

        acceptor->async_accept(
            [&](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    auto socket_ptr =
                        std::make_shared<tcp::socket>(
                            std::move(socket));

                    auto buffer =
                        std::make_shared<beast::flat_buffer>();

                    auto request =
                        std::make_shared<
                            http::request<http::string_body>>();

                    http::async_read(
                        *socket_ptr,
                        *buffer,
                        *request,
                        [socket_ptr, buffer, request](
                            beast::error_code ec,
                            std::size_t)
                        {
                            if (ec)
                                return;

                            auto response =
                                std::make_shared<
                                    http::response<http::string_body>>(
                                    http::status::ok,
                                    request->version());

                            response->set(
                                http::field::content_type,
                                "text/plain");

                            response->body() = "backend";
                            response->prepare_payload();
                            response->keep_alive(false);

                            http::async_write(
                                *socket_ptr,
                                *response,
                                [socket_ptr](
                                    beast::error_code,
                                    std::size_t)
                                {
                                    beast::error_code shutdown_ec;

                                    socket_ptr->shutdown(
                                        tcp::socket::shutdown_both,
                                        shutdown_ec);
                                });
                        });

                    // Continue accepting connections.
                    do_accept();
                }
                else if (ec != asio::error::operation_aborted)
                {
                    // If the acceptor failed for a reason other than shutdown,
                    // try accepting again.
                    do_accept();
                }
            });
    };

    do_accept();

    // -------------------------------------------------------------------------
    // Gateway dependencies
    // -------------------------------------------------------------------------

    std::vector<Backend> backends;

    backends.emplace_back(
        "127.0.0.1",
        backend_port,
        5,
        std::chrono::milliseconds(100));

    auto load_balancer =
        std::make_shared<LoadBalancer>(
            std::move(backends));

    auto metrics =
        std::make_shared<Metrics>();

    auto rate_limit_store =
        std::make_unique<InMemoryRateLimitStore>();

    // Capacity = 2 and refill rate = 0.
    //
    // Request 1 -> allowed
    // Request 2 -> allowed
    // Request 3 -> rejected
    //
    // We use:
    //
    // Request 1 -> 200
    // Request 2 -> 503
    // Request 3 -> 429
    //
    // This lets us test both the 503 and 429 latency paths without
    // needing a reset operation on the in-memory rate limiter.

    auto rate_limiter =
        std::make_shared<RateLimiter>(
            *rate_limit_store,
            2.0,
            0.0);

    GatewayConfig config;

    config.port = gateway_port;

    config.backends = {
        Backend(
            "127.0.0.1",
            backend_port,
            5,
            std::chrono::milliseconds(100))};

    config.rate_limit_capacity = 2.0;
    config.rate_limit_refill_rate = 0.0;

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        true);

    // -------------------------------------------------------------------------
    // Start gateway/backend event loop.
    // -------------------------------------------------------------------------

    std::thread io_thread(
        [&io_context]()
        {
            io_context.run();
        });

    // Give the acceptors a moment to start listening.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));

    // -------------------------------------------------------------------------
    // TEST 1: Normal request -> 200
    // -------------------------------------------------------------------------

    std::cerr << "[TEST] Starting normal request\n";

    std::string response =
        send_request(
            gateway_port,
            "/",
            200);

    std::cerr << "[TEST] Normal request completed\n";

    assert(response == "backend");

    // Request counters.
    assert(metrics->requests_total() == 1);

    // Backend counters.
    assert(metrics->backend_requests_total() == 1);
    assert(metrics->backend_successes_total() == 1);
    assert(metrics->backend_failures_total() == 0);

    // HTTP response counters.
    assert(metrics->responses_2xx_total() == 1);
    assert(metrics->responses_4xx_total() == 0);
    assert(metrics->responses_5xx_total() == 0);

    // Rate limiting.
    assert(metrics->rate_limit_allowed_total() == 1);
    assert(metrics->rate_limit_rejected_total() == 0);

    // The real HTTP request must have recorded request latency.
    //
    // Latency is recorded from the async_write completion handler,
    // so wait for that asynchronous metric update before asserting.
    assert(wait_for_request_duration_count(metrics, 1));
    assert(metrics->request_duration_sum_us() > 0);

    std::cout
        << "[PASS] normal request establishes metrics\n";

    // -------------------------------------------------------------------------
    // TEST 2: Backend unavailable -> 503
    // -------------------------------------------------------------------------

    std::cerr << "[TEST] Marking backend unhealthy\n";

    // Prevent the load balancer from selecting the backend.
    assert(!load_balancer->backends().empty());

    load_balancer->backends()[0].healthy = false;

    std::cerr << "[TEST] Starting 503 request\n";

    response =
        send_request(
            gateway_port,
            "/",
            503);

    std::cerr << "[TEST] 503 request completed\n";

    // The request itself must have been counted.
    assert(metrics->requests_total() == 2);

    // The first request reached the backend.
    // The second request was allowed through the rate limiter but
    // could not find a healthy backend.
    assert(metrics->backend_requests_total() == 1);
    assert(metrics->backend_successes_total() == 1);

    // We now have:
    //   1 x 2xx
    //   0 x 4xx
    //   1 x 5xx
    assert(metrics->responses_2xx_total() == 1);
    assert(metrics->responses_4xx_total() == 0);
    assert(metrics->responses_5xx_total() == 1);

    // Both requests were allowed by the rate limiter.
    assert(metrics->rate_limit_allowed_total() == 2);
    assert(metrics->rate_limit_rejected_total() == 0);

    // The 503 response must be included in request latency.
    assert(wait_for_request_duration_count(metrics, 2));
    assert(metrics->request_duration_sum_us() > 0);

    std::cout
        << "[PASS] 503 response records request latency\n";

    // -------------------------------------------------------------------------
    // TEST 3: Rate-limited request -> 429
    // -------------------------------------------------------------------------

    std::cerr << "[TEST] Starting rate-limited request\n";

    response =
        send_request(
            gateway_port,
            "/",
            429);

    std::cerr << "[TEST] Rate-limited request completed\n";

    // The request itself must have been counted.
    assert(metrics->requests_total() == 3);

    // It must NOT have reached the backend.
    assert(metrics->backend_requests_total() == 1);
    assert(metrics->backend_successes_total() == 1);

    // We now have:
    //   1 x 2xx
    //   1 x 4xx
    //   1 x 5xx
    assert(metrics->responses_2xx_total() == 1);
    assert(metrics->responses_4xx_total() == 1);
    assert(metrics->responses_5xx_total() == 1);

    // Two requests were allowed and the third was rejected.
    assert(metrics->rate_limit_allowed_total() == 2);
    assert(metrics->rate_limit_rejected_total() == 1);

    // The 429 response must also be included in request latency.
    assert(wait_for_request_duration_count(metrics, 3));
    assert(metrics->request_duration_sum_us() > 0);

    std::cout
        << "[PASS] 429 response records request latency\n";

    // -------------------------------------------------------------------------
    // TEST 4: /metrics bypasses normal request pipeline
    // -------------------------------------------------------------------------

    std::cerr << "[TEST] Starting /metrics request\n";

    response =
        send_request(
            gateway_port,
            "/metrics",
            200);

    std::cerr << "[TEST] /metrics request completed\n";

    // -------------------------------------------------------------------------
    // Verify normal counters are exposed.
    // -------------------------------------------------------------------------

    assert(
        response.find(
            "gateway_requests_total 3") != std::string::npos);

    assert(
        response.find(
            "gateway_backend_requests_total 1") != std::string::npos);

    assert(
        response.find(
            "gateway_backend_successes_total 1") != std::string::npos);

    assert(
        response.find(
            "gateway_responses_2xx_total 1") != std::string::npos);

    assert(
        response.find(
            "gateway_responses_4xx_total 1") != std::string::npos);

    assert(
        response.find(
            "gateway_responses_5xx_total 1") != std::string::npos);

    // -------------------------------------------------------------------------
    // Verify latency histogram.
    // -------------------------------------------------------------------------

    // Exactly three real HTTP requests were processed.
    assert(
        response.find(
            "gateway_request_duration_seconds_count 3\n") != std::string::npos);

    // All three observations must be included in +Inf.
    assert(
        response.find(
            "gateway_request_duration_seconds_bucket{le=\"+Inf\"} 3\n") != std::string::npos);

    // -------------------------------------------------------------------------
    // /metrics itself must NOT modify request metrics.
    // -------------------------------------------------------------------------

    assert(metrics->requests_total() == 3);

    assert(metrics->backend_requests_total() == 1);
    assert(metrics->backend_successes_total() == 1);

    assert(metrics->responses_2xx_total() == 1);
    assert(metrics->responses_4xx_total() == 1);
    assert(metrics->responses_5xx_total() == 1);

    // The histogram must still contain exactly three real requests.
    //
    // The /metrics request intentionally does not contribute a latency
    // observation.
    assert(wait_for_request_duration_count(metrics, 3));
    assert(metrics->request_duration_count() == 3);

    // -------------------------------------------------------------------------
    // /metrics must bypass rate limiting.
    // -------------------------------------------------------------------------

    assert(metrics->rate_limit_allowed_total() == 2);
    assert(metrics->rate_limit_rejected_total() == 1);

    std::cout
        << "[PASS] /metrics bypasses request pipeline\n";

    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------

    std::cerr << "[TEST] Starting cleanup\n";

    // Explicitly cancel the backend accept operation.
    beast::error_code ec;

    if (acceptor->is_open())
    {
        acceptor->close(ec);
    }

    std::cerr << "[TEST] Backend acceptor closed\n";

    // Stop the event loop.
    io_context.stop();

    std::cerr << "[TEST] io_context stopped\n";

    // Wait for the event-loop thread to finish.
    if (io_thread.joinable())
    {
        io_thread.join();
    }

    std::cerr << "[TEST] io_thread joined\n";

    std::cout
        << "\nAll metrics endpoint integration tests passed.\n";

    return 0;
}