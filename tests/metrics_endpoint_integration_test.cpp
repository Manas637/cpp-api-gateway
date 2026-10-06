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
#include "metrics.hpp"
#include "rate_limiter.hpp"
#include "server.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

// =============================================================================
// Constants
// =============================================================================

namespace
{
    constexpr unsigned short gateway_port = 18082;
    constexpr unsigned short backend_port = 19004;

    // =============================================================================
    // HTTP client helper
    // =============================================================================

    std::string send_request(
        unsigned short port,
        const std::string &target,
        unsigned short expected_status)
    {
        asio::io_context io_context;

        tcp::resolver resolver(io_context);
        tcp::socket socket(io_context);

        auto endpoints = resolver.resolve(
            "127.0.0.1",
            std::to_string(port));

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

        http::read(
            socket,
            buffer,
            response);

        assert(
            response.result_int() ==
            expected_status);

        return response.body();
    }

    // =============================================================================
    // Wait for asynchronously recorded request latency
    // =============================================================================

    bool wait_for_request_duration_count(
        const std::shared_ptr<Metrics> &metrics,
        std::uint64_t expected_count,
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds(2000))
    {
        const auto deadline =
            std::chrono::steady_clock::now() +
            timeout;

        while (
            std::chrono::steady_clock::now() <
            deadline)
        {
            if (
                metrics->request_duration_count() >=
                expected_count)
            {
                return true;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }

        return (
            metrics->request_duration_count() >=
            expected_count);
    }

    // =============================================================================
    // Fake backend session
    // =============================================================================

    class BackendSession
        : public std::enable_shared_from_this<BackendSession>
    {
    private:
        tcp::socket socket_;
        beast::flat_buffer buffer_;
        http::request<http::string_body> request_;

    public:
        explicit BackendSession(
            tcp::socket socket)
            : socket_(std::move(socket))
        {
        }

        void start()
        {
            read_request();
        }

    private:
        void read_request()
        {
            auto self = shared_from_this();

            http::async_read(
                socket_,
                buffer_,
                request_,
                [self](
                    beast::error_code ec,
                    std::size_t)
                {
                    if (ec)
                    {
                        return;
                    }

                    self->send_response();
                });
        }

        void send_response()
        {
            auto self = shared_from_this();

            auto response =
                std::make_shared<
                    http::response<http::string_body>>(
                    http::status::ok,
                    request_.version());

            response->set(
                http::field::content_type,
                "text/plain");

            response->body() = "backend";
            response->prepare_payload();

            response->keep_alive(false);

            http::async_write(
                socket_,
                *response,
                [self, response](
                    beast::error_code ec,
                    std::size_t)
                {
                    beast::error_code shutdown_ec;

                    self->socket_.shutdown(
                        tcp::socket::shutdown_both,
                        shutdown_ec);

                    self->socket_.close(
                        shutdown_ec);

                    (void)ec;
                });
        }
    };

    // =============================================================================
    // Fake backend server
    // =============================================================================

    class FakeBackend
    {
    private:
        asio::io_context io_context_;

        tcp::acceptor acceptor_;

    public:
        FakeBackend()
            : acceptor_(
                  io_context_,
                  tcp::endpoint(
                      tcp::v4(),
                      backend_port))
        {
        }

        void start()
        {
            accept();

            thread_ = std::thread(
                [this]()
                {
                    io_context_.run();
                });
        }

        void stop()
        {
            beast::error_code ec;

            acceptor_.close(ec);

            io_context_.stop();

            if (thread_.joinable())
            {
                thread_.join();
            }
        }

    private:
        std::thread thread_;

        void accept()
        {
            acceptor_.async_accept(
                [this](
                    beast::error_code ec,
                    tcp::socket socket)
                {
                    if (!ec)
                    {
                        std::make_shared<
                            BackendSession>(
                            std::move(socket))
                            ->start();
                    }

                    if (
                        ec !=
                        asio::error::operation_aborted)
                    {
                        accept();
                    }
                });
        }
    };

    // =============================================================================
    // Wait until a TCP endpoint is accepting connections
    // =============================================================================

    bool wait_for_port(
        unsigned short port,
        std::chrono::milliseconds timeout =
            std::chrono::milliseconds(2000))
    {
        const auto deadline =
            std::chrono::steady_clock::now() +
            timeout;

        while (
            std::chrono::steady_clock::now() <
            deadline)
        {
            try
            {
                asio::io_context io_context;

                tcp::socket socket(io_context);

                tcp::endpoint endpoint(
                    asio::ip::make_address(
                        "127.0.0.1"),
                    port);

                socket.connect(endpoint);

                beast::error_code ec;

                socket.shutdown(
                    tcp::socket::shutdown_both,
                    ec);

                socket.close(ec);

                return true;
            }
            catch (...)
            {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
        }

        return false;
    }

} // namespace

// =============================================================================
// Main test
// =============================================================================

int main()
{
    std::cerr
        << "[TEST] Starting MetricsEndpointIntegrationTest\n";

    // -------------------------------------------------------------------------
    // Start fake backend
    // -------------------------------------------------------------------------

    FakeBackend backend;

    backend.start();

    assert(
        wait_for_port(
            backend_port));

    std::cerr
        << "[TEST] Backend is ready\n";

    // -------------------------------------------------------------------------
    // Gateway
    // -------------------------------------------------------------------------

    asio::io_context gateway_io_context;

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
        std::make_unique<
            InMemoryRateLimitStore>();

    // Capacity = 2, refill = 0:
    //
    // Request 1 -> allowed
    // Request 2 -> allowed
    // Request 3 -> rejected

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
        gateway_io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        true);

    // -------------------------------------------------------------------------
    // Start gateway event loop
    // -------------------------------------------------------------------------

    std::thread gateway_thread(
        [&gateway_io_context]()
        {
            gateway_io_context.run();
        });

    assert(
        wait_for_port(
            gateway_port));

    std::cerr
        << "[TEST] Gateway is ready\n";

    // =========================================================================
    // TEST 1: Normal request -> 200
    // =========================================================================

    std::cerr
        << "[TEST] Starting normal request\n";

    std::string response =
        send_request(
            gateway_port,
            "/",
            200);

    std::cerr
        << "[TEST] Normal request completed\n";

    assert(
        response == "backend");

    // Request counters
    assert(
        metrics->requests_total() == 1);

    // Backend counters
    assert(
        metrics->backend_requests_total() == 1);

    assert(
        metrics->backend_successes_total() == 1);

    assert(
        metrics->backend_failures_total() == 0);

    // HTTP response counters
    assert(
        metrics->responses_2xx_total() == 1);

    assert(
        metrics->responses_4xx_total() == 0);

    assert(
        metrics->responses_5xx_total() == 0);

    // Rate limiting
    assert(
        metrics->rate_limit_allowed_total() == 1);

    assert(
        metrics->rate_limit_rejected_total() == 0);

    // Latency
    assert(
        wait_for_request_duration_count(
            metrics,
            1));

    assert(
        metrics->request_duration_sum_us() > 0);

    std::cout
        << "[PASS] normal request establishes metrics\n";

    // =========================================================================
    // TEST 2: Backend unavailable -> 503
    // =========================================================================

    std::cerr
        << "[TEST] Marking backend unhealthy\n";

    assert(
        !load_balancer->backends().empty());

    load_balancer->backends()[0].healthy =
        false;

    std::cerr
        << "[TEST] Starting 503 request\n";

    response =
        send_request(
            gateway_port,
            "/",
            503);

    std::cerr
        << "[TEST] 503 request completed\n";

    // Request counter
    assert(
        metrics->requests_total() == 2);

    // Only first request reached backend
    assert(
        metrics->backend_requests_total() == 1);

    assert(
        metrics->backend_successes_total() == 1);

    assert(
        metrics->backend_failures_total() == 0);

    // Responses:
    // 1 x 2xx
    // 0 x 4xx
    // 1 x 5xx

    assert(
        metrics->responses_2xx_total() == 1);

    assert(
        metrics->responses_4xx_total() == 0);

    assert(
        metrics->responses_5xx_total() == 1);

    // Both requests allowed
    assert(
        metrics->rate_limit_allowed_total() == 2);

    assert(
        metrics->rate_limit_rejected_total() == 0);

    // Latency
    assert(
        wait_for_request_duration_count(
            metrics,
            2));

    assert(
        metrics->request_duration_sum_us() > 0);

    std::cout
        << "[PASS] 503 response records request latency\n";

    // =========================================================================
    // TEST 3: Rate-limited request -> 429
    // =========================================================================

    std::cerr
        << "[TEST] Starting rate-limited request\n";

    response =
        send_request(
            gateway_port,
            "/",
            429);

    std::cerr
        << "[TEST] Rate-limited request completed\n";

    // Request counter
    assert(
        metrics->requests_total() == 3);

    // Must not reach backend
    assert(
        metrics->backend_requests_total() == 1);

    assert(
        metrics->backend_successes_total() == 1);

    // Responses:
    // 1 x 2xx
    // 1 x 4xx
    // 1 x 5xx

    assert(
        metrics->responses_2xx_total() == 1);

    assert(
        metrics->responses_4xx_total() == 1);

    assert(
        metrics->responses_5xx_total() == 1);

    // Two allowed, one rejected
    assert(
        metrics->rate_limit_allowed_total() == 2);

    assert(
        metrics->rate_limit_rejected_total() == 1);

    // Latency
    assert(
        wait_for_request_duration_count(
            metrics,
            3));

    assert(
        metrics->request_duration_sum_us() > 0);

    std::cout
        << "[PASS] 429 response records request latency\n";

    // =========================================================================
    // TEST 4: /metrics bypasses normal request pipeline
    // =========================================================================

    std::cerr
        << "[TEST] Starting /metrics request\n";

    response =
        send_request(
            gateway_port,
            "/metrics",
            200);

    std::cerr
        << "[TEST] /metrics request completed\n";

    // -------------------------------------------------------------------------
    // Verify normal counters are exposed
    // -------------------------------------------------------------------------

    assert(
        response.find(
            "gateway_requests_total 3") !=
        std::string::npos);

    assert(
        response.find(
            "gateway_backend_requests_total 1") !=
        std::string::npos);

    assert(
        response.find(
            "gateway_backend_successes_total 1") !=
        std::string::npos);

    assert(
        response.find(
            "gateway_responses_2xx_total 1") !=
        std::string::npos);

    assert(
        response.find(
            "gateway_responses_4xx_total 1") !=
        std::string::npos);

    assert(
        response.find(
            "gateway_responses_5xx_total 1") !=
        std::string::npos);

    // -------------------------------------------------------------------------
    // Verify latency histogram
    // -------------------------------------------------------------------------

    assert(
        response.find(
            "gateway_request_duration_seconds_count 3\n") !=
        std::string::npos);

    assert(
        response.find(
            "gateway_request_duration_seconds_bucket{le=\"+Inf\"} 3\n") !=
        std::string::npos);

    // -------------------------------------------------------------------------
    // /metrics must not modify normal request metrics
    // -------------------------------------------------------------------------

    assert(
        metrics->requests_total() == 3);

    assert(
        metrics->backend_requests_total() == 1);

    assert(
        metrics->backend_successes_total() == 1);

    assert(
        metrics->responses_2xx_total() == 1);

    assert(
        metrics->responses_4xx_total() == 1);

    assert(
        metrics->responses_5xx_total() == 1);

    // /metrics itself does not record request latency.
    assert(
        wait_for_request_duration_count(
            metrics,
            3));

    assert(
        metrics->request_duration_count() == 3);

    // -------------------------------------------------------------------------
    // /metrics must bypass rate limiting
    // -------------------------------------------------------------------------

    assert(
        metrics->rate_limit_allowed_total() == 2);

    assert(
        metrics->rate_limit_rejected_total() == 1);

    std::cout
        << "[PASS] /metrics bypasses request pipeline\n";

    // =========================================================================
    // Cleanup
    // =========================================================================

    std::cerr
        << "[TEST] Starting cleanup\n";

    // Stop gateway first.
    gateway_io_context.stop();

    if (gateway_thread.joinable())
    {
        gateway_thread.join();
    }

    std::cerr
        << "[TEST] Gateway thread joined\n";

    // Then stop fake backend.
    backend.stop();

    std::cerr
        << "[TEST] Backend stopped\n";

    std::cout
        << "\nAll metrics endpoint integration tests passed.\n";

    return 0;
}