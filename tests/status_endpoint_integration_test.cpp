#include <cassert>
#include <chrono>
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
#include "metrics.hpp"
#include "rate_limiter.hpp"
#include "server.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

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
    constexpr unsigned short gateway_port = 18083;

    asio::io_context io_context;

    // -------------------------------------------------------------------------
    // Gateway dependencies
    // -------------------------------------------------------------------------

    std::vector<Backend> backends;

    backends.emplace_back(
        "127.0.0.1",
        19001);

    backends.emplace_back(
        "127.0.0.1",
        19002);

    backends.emplace_back(
        "127.0.0.1",
        19003);

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
            100.0,
            100.0);

    GatewayConfig config;

    config.port = gateway_port;

    config.backends = {
        Backend("127.0.0.1", 19001),
        Backend("127.0.0.1", 19002),
        Backend("127.0.0.1", 19003)};

    config.rate_limit_capacity = 100.0;
    config.rate_limit_refill_rate = 100.0;

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        true);

    // -------------------------------------------------------------------------
    // Start gateway
    // -------------------------------------------------------------------------

    std::thread io_thread(
        [&io_context]()
        {
            io_context.run();
        });

    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));

    // -------------------------------------------------------------------------
    // TEST 1: All backends healthy
    // -------------------------------------------------------------------------

    std::cerr
        << "[TEST] Checking healthy gateway status\n";

    std::string response =
        send_request(
            gateway_port,
            "/api/status",
            200);

    assert(
        response.find(
            "\"status\":\"healthy\"") !=
        std::string::npos);

    assert(
        response.find(
            "\"active_connections\":") !=
        std::string::npos);

    assert(
        response.find(
            "\"rate_limiting\":{\"enabled\":true}") !=
        std::string::npos);

    assert(
        response.find(
            "\"port\":19001") !=
        std::string::npos);

    assert(
        response.find(
            "\"port\":19002") !=
        std::string::npos);

    assert(
        response.find(
            "\"port\":19003") !=
        std::string::npos);

    assert(
        response.find(
            "\"healthy\":true") !=
        std::string::npos);

    assert(
        response.find(
            "\"circuit\":\"CLOSED\"") !=
        std::string::npos);

    std::cout
        << "[PASS] healthy status\n";

    // -------------------------------------------------------------------------
    // Verify /api/status bypasses normal request pipeline
    // -------------------------------------------------------------------------

    assert(metrics->requests_total() == 0);
    assert(metrics->backend_requests_total() == 0);
    assert(metrics->rate_limit_allowed_total() == 0);
    assert(metrics->rate_limit_rejected_total() == 0);

    std::cout
        << "[PASS] status endpoint bypasses request pipeline\n";

    // -------------------------------------------------------------------------
    // TEST 2: One backend unhealthy -> degraded
    // -------------------------------------------------------------------------

    std::cerr
        << "[TEST] Marking backend 9002 unhealthy\n";

    load_balancer->backends()[1].healthy = false;

    response =
        send_request(
            gateway_port,
            "/api/status",
            200);

    assert(
        response.find(
            "\"status\":\"degraded\"") !=
        std::string::npos);

    assert(
        response.find(
            "\"port\":19002,\"healthy\":false") !=
        std::string::npos);

    std::cout
        << "[PASS] degraded status\n";

    // -------------------------------------------------------------------------
    // TEST 3: All backends unhealthy -> unhealthy
    // -------------------------------------------------------------------------

    std::cerr
        << "[TEST] Marking all backends unhealthy\n";

    load_balancer->backends()[0].healthy = false;
    load_balancer->backends()[2].healthy = false;

    response =
        send_request(
            gateway_port,
            "/api/status",
            200);

    assert(
        response.find(
            "\"status\":\"unhealthy\"") !=
        std::string::npos);

    std::cout
        << "[PASS] unhealthy status\n";

    // -------------------------------------------------------------------------
    // Verify status endpoint never entered normal request pipeline
    // -------------------------------------------------------------------------

    assert(metrics->requests_total() == 0);
    assert(metrics->backend_requests_total() == 0);
    assert(metrics->backend_successes_total() == 0);
    assert(metrics->backend_failures_total() == 0);

    assert(metrics->rate_limit_allowed_total() == 0);
    assert(metrics->rate_limit_rejected_total() == 0);

    assert(metrics->responses_2xx_total() == 0);
    assert(metrics->responses_4xx_total() == 0);
    assert(metrics->responses_5xx_total() == 0);

    std::cout
        << "[PASS] status endpoint does not modify gateway metrics\n";

    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------

    std::cerr
        << "[TEST] Starting cleanup\n";

    io_context.stop();

    if (io_thread.joinable())
    {
        io_thread.join();
    }

    std::cout
        << "\nAll status endpoint integration tests passed.\n";

    return 0;
}