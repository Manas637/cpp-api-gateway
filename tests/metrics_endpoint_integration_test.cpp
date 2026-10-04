#include <cassert>
#include <chrono>
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

    auto rate_limiter =
        std::make_shared<RateLimiter>(
            *rate_limit_store,
            1.0,
            0.0);

    GatewayConfig config;

    config.port = gateway_port;

    config.backends = {
        Backend(
            "127.0.0.1",
            backend_port,
            5,
            std::chrono::milliseconds(100))};

    config.rate_limit_capacity = 1.0;
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
    // TEST 1: Normal request
    // -------------------------------------------------------------------------

    std::cerr << "[TEST] Starting normal request\n";

    std::string response =
        send_request(
            gateway_port,
            "/",
            200);

    std::cerr << "[TEST] Normal request completed\n";

    assert(response == "backend");

    assert(metrics->requests_total() == 1);
    assert(metrics->backend_requests_total() == 1);
    assert(metrics->backend_successes_total() == 1);
    assert(metrics->responses_2xx_total() == 1);
    assert(metrics->rate_limit_allowed_total() == 1);
    assert(metrics->rate_limit_rejected_total() == 0);

    std::cout
        << "[PASS] normal request establishes metrics\n";

    // -------------------------------------------------------------------------
    // TEST 2: /metrics bypasses normal request pipeline
    // -------------------------------------------------------------------------

    std::cerr << "[TEST] Starting /metrics request\n";

    response =
        send_request(
            gateway_port,
            "/metrics",
            200);

    std::cerr << "[TEST] /metrics request completed\n";

    assert(
        response.find("gateway_requests_total 1") != std::string::npos);

    assert(
        response.find("gateway_backend_requests_total 1") != std::string::npos);

    assert(
        response.find("gateway_backend_successes_total 1") != std::string::npos);

    assert(
        response.find("gateway_responses_2xx_total 1") != std::string::npos);

    // The metrics scrape itself must NOT modify these.
    assert(metrics->requests_total() == 1);
    assert(metrics->backend_requests_total() == 1);
    assert(metrics->backend_successes_total() == 1);
    assert(metrics->responses_2xx_total() == 1);

    // /metrics must bypass rate limiting.
    assert(metrics->rate_limit_allowed_total() == 1);
    assert(metrics->rate_limit_rejected_total() == 0);

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