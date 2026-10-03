#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

// ============================================================
// Test Backend
// ============================================================

class TestBackend
{
public:
    TestBackend(
        asio::io_context &io_context,
        unsigned short port)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), port))
    {
        accept();
    }

private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;

    void accept()
    {
        acceptor_.async_accept(
            [this](
                beast::error_code ec,
                tcp::socket socket)
            {
                if (!ec)
                {
                    handle_connection(std::move(socket));
                }

                if (acceptor_.is_open())
                {
                    accept();
                }
            });
    }

    void handle_connection(tcp::socket socket)
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
                {
                    return;
                }

                auto response =
                    std::make_shared<
                        http::response<http::string_body>>();

                response->version(
                    request->version());

                response->result(
                    http::status::ok);

                response->set(
                    http::field::server,
                    "RateLimitTestBackend");

                response->set(
                    http::field::content_type,
                    "text/plain");

                response->body() =
                    "backend reached";

                response->prepare_payload();

                response->set(
                    http::field::connection,
                    "close");

                http::async_write(
                    *socket_ptr,
                    *response,
                    [socket_ptr, response](
                        beast::error_code,
                        std::size_t)
                    {
                        beast::error_code shutdown_ec;

                        socket_ptr->shutdown(
                            tcp::socket::shutdown_both,
                            shutdown_ec);

                        socket_ptr->close(
                            shutdown_ec);
                    });
            });
    }
};

// ============================================================
// HTTP Client Helper
// ============================================================

std::string send_request(
    unsigned short gateway_port,
    unsigned int expected_status)
{
    asio::io_context io_context;

    tcp::resolver resolver(io_context);
    tcp::socket socket(io_context);

    auto endpoints =
        resolver.resolve(
            "127.0.0.1",
            std::to_string(gateway_port));

    asio::connect(
        socket,
        endpoints);

    http::request<http::string_body> request{
        http::verb::get,
        "/",
        11};

    request.set(
        http::field::host,
        "localhost");

    request.set(
        http::field::connection,
        "close");

    http::write(
        socket,
        request);

    beast::flat_buffer buffer;

    http::response<http::string_body> response;

    http::read(
        socket,
        buffer,
        response);

    assert(
        response.result_int() ==
        expected_status);

    return response.body();
}

// ============================================================
// Main Integration Test
// ============================================================

int main()
{
    constexpr unsigned short gateway_port = 18081;
    constexpr unsigned short backend_port = 19003;

    asio::io_context io_context;

    // ========================================================
    // Test Backend
    // ========================================================

    TestBackend backend(
        io_context,
        backend_port);

    // ========================================================
    // Load Balancer
    // ========================================================

    std::vector<Backend> backends;

    backends.emplace_back(
        "127.0.0.1",
        backend_port,
        5,
        std::chrono::milliseconds(100));

    auto load_balancer =
        std::make_shared<LoadBalancer>(
            std::move(backends));

    // ========================================================
    // Metrics
    // ========================================================

    auto metrics =
        std::make_shared<Metrics>();

    // ========================================================
    // Rate Limiter
    // ========================================================

    auto rate_limit_store =
        std::make_unique<InMemoryRateLimitStore>();

    auto rate_limiter =
        std::make_shared<RateLimiter>(
            *rate_limit_store,
            2.0,
            0.1);

    // ========================================================
    // Gateway Configuration
    // ========================================================

    GatewayConfig config;

    config.port = gateway_port;

    config.backends = {
        Backend(
            "127.0.0.1",
            backend_port,
            5,
            std::chrono::milliseconds(100))};

    config.rate_limit_capacity = 2.0;
    config.rate_limit_refill_rate = 0.1;

    // ========================================================
    // Gateway Server
    // ========================================================

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        true);

    std::thread io_thread(
        [&io_context]()
        {
            io_context.run();
        });

    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));

    // ========================================================
    // TEST
    // Two allowed + two rejected
    // ========================================================

    std::string response;

    response =
        send_request(
            gateway_port,
            200);

    assert(
        response == "backend reached");

    response =
        send_request(
            gateway_port,
            200);

    assert(
        response == "backend reached");

    response =
        send_request(
            gateway_port,
            429);

    response =
        send_request(
            gateway_port,
            429);

    // ========================================================
    // Metrics Assertions
    // ========================================================

    assert(
        metrics->requests_total() == 4);

    assert(
        metrics->rate_limit_allowed_total() == 2);

    assert(
        metrics->rate_limit_rejected_total() == 2);

    assert(
        metrics->backend_requests_total() == 2);

    assert(
        metrics->backend_successes_total() == 2);

    assert(
        metrics->backend_failures_total() == 0);

    assert(
        metrics->responses_2xx_total() == 2);

    assert(
        metrics->responses_4xx_total() == 2);

    assert(
        metrics->responses_5xx_total() == 0);

    std::cout
        << "[PASS] rate-limit metrics integration\n";

    // ========================================================
    // Cleanup
    // ========================================================

    io_context.stop();

    if (io_thread.joinable())
    {
        io_thread.join();
    }

    std::cout
        << "\nRate-limit metrics integration test passed.\n";

    return 0;
}