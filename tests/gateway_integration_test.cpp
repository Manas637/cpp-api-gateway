#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <cstdint>

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

// ============================================================
// Test Backend
// ============================================================

class TestBackend
{
public:
    TestBackend(
        asio::io_context &io_context,
        unsigned short port,
        std::string response_body)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), port)),
          response_body_(std::move(response_body))
    {
        accept();
    }

    void set_failing(bool failing)
    {
        failing_.store(failing);
    }

private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;

    std::string response_body_;

    std::atomic_bool failing_{false};

    void accept()
    {
        acceptor_.async_accept(
            [this](
                beast::error_code ec,
                tcp::socket socket)
            {
                if (!ec)
                {
                    handle_connection(
                        std::move(socket));
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

            [this, socket_ptr, buffer, request](
                beast::error_code ec,
                std::size_t)
            {
                if (ec)
                {
                    return;
                }

                // Simulate upstream failure by closing the
                // connection without sending a response.
                if (failing_.load())
                {
                    beast::error_code shutdown_ec;

                    socket_ptr->shutdown(
                        tcp::socket::shutdown_both,
                        shutdown_ec);

                    socket_ptr->close(shutdown_ec);

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
                    "TestBackend");

                response->set(
                    http::field::content_type,
                    "text/plain");

                response->body() =
                    response_body_;

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
    unsigned short expected_status = 200)
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

    // Each test request uses a fresh client connection.
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

void assert_metric(
    const Metrics &metrics,
    uint64_t requests,
    uint64_t backend_requests,
    uint64_t backend_successes,
    uint64_t backend_failures,
    uint64_t failovers,
    uint64_t circuit_opens,
    uint64_t circuit_half_opens,
    uint64_t circuit_recoveries)
{
    assert(metrics.requests_total() == requests);
    assert(metrics.backend_requests_total() == backend_requests);
    assert(metrics.backend_successes_total() == backend_successes);
    assert(metrics.backend_failures_total() == backend_failures);
    assert(metrics.failovers_total() == failovers);
    assert(metrics.circuit_opens_total() == circuit_opens);
    assert(metrics.circuit_half_opens_total() == circuit_half_opens);
    assert(metrics.circuit_recoveries_total() == circuit_recoveries);
}

// ============================================================
// Main Integration Test
// ============================================================

int main()
{
    constexpr unsigned short gateway_port = 18080;

    constexpr unsigned short backend_a_port = 19001;

    constexpr unsigned short backend_b_port = 19002;

    constexpr std::size_t failure_threshold = 5;

    // Short duration for deterministic testing.
    // Production uses a longer duration.
    constexpr auto open_duration =
        std::chrono::milliseconds(100);

    asio::io_context io_context;

    // ========================================================
    // Test Backends
    // ========================================================

    TestBackend backend_a(
        io_context,
        backend_a_port,
        "backend A");

    TestBackend backend_b(
        io_context,
        backend_b_port,
        "backend B");

    // ========================================================
    // Load Balancer
    // ========================================================

    std::vector<Backend> backends;

    backends.emplace_back(
        "127.0.0.1",
        backend_a_port,
        failure_threshold,
        open_duration);

    backends.emplace_back(
        "127.0.0.1",
        backend_b_port,
        failure_threshold,
        open_duration);

    auto load_balancer =
        std::make_shared<LoadBalancer>(
            std::move(backends));

    auto metrics = std::make_shared<Metrics>();

    // ========================================================
    // Rate Limiter
    // ========================================================

    auto rate_limit_store =
        std::make_unique<InMemoryRateLimitStore>();

    auto rate_limiter =
        std::make_shared<RateLimiter>(
            *rate_limit_store,
            1000.0,
            1000.0);

    // ========================================================
    // Gateway Configuration
    // ========================================================

    GatewayConfig config;

    config.port = gateway_port;

    config.backends = {
        Backend(
            "127.0.0.1",
            backend_a_port,
            failure_threshold,
            open_duration),

        Backend(
            "127.0.0.1",
            backend_b_port,
            failure_threshold,
            open_duration)};

    config.rate_limit_capacity = 1000.0;

    config.rate_limit_refill_rate = 1000.0;

    // ========================================================
    // Gateway Server
    // ========================================================

    // Server starts accepting connections in its constructor.
    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        false);

    // Give the server/backend acceptors time to start.
    std::thread io_thread(
        [&io_context]()
        {
            io_context.run();
        });

    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));

    // ========================================================
    // TEST 1
    // Normal round-robin routing
    // ========================================================

    {
        std::string response =
            send_request(gateway_port);

        assert(response == "backend A");

        response =
            send_request(gateway_port);

        assert(response == "backend B");

        assert_metric(
            *metrics,
            2,  // requests
            2,  // backend requests
            2,  // backend successes
            0,  // backend failures
            0,  // failovers
            0,  // circuit opens
            0,  // circuit half-opens
            0); // recoveries

        std::cout
            << "[PASS] round-robin routing\n";
    }

    // ========================================================
    // TEST 2
    // Backend A failure -> failover to B
    // ========================================================

    backend_a.set_failing(true);

    {
        std::string response =
            send_request(gateway_port);

        // A fails, gateway retries B.
        assert(response == "backend B");

        assert_metric(
            *metrics,
            3,
            4,
            3,
            1,
            1,
            0,
            0,
            0);

        std::cout
            << "[PASS] failover after backend failure\n";
    }

    // ========================================================
    // TEST 3
    // Repeated A failures -> circuit OPEN
    // ========================================================

    {
        for (std::size_t i = 0;
             i < failure_threshold;
             ++i)
        {
            std::string response =
                send_request(gateway_port);

            // A fails and B handles the retry.
            assert(response == "backend B");
        }

        Backend &backend_a_state =
            load_balancer->backends()[0];

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::OPEN);

        assert_metric(
            *metrics,
            8,
            13,
            8,
            5,
            5,
            1,
            0,
            0);

        std::cout
            << "[PASS] circuit opens after repeated failures\n";
    }

    // ========================================================
    // TEST 4
    // OPEN backend is skipped
    // ========================================================

    {
        std::string response =
            send_request(gateway_port);

        // A is OPEN, so load balancer skips it.
        assert(response == "backend B");

        Backend &backend_a_state =
            load_balancer->backends()[0];

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::OPEN);

        assert_metric(
            *metrics,
            9,
            14,
            9,
            5,
            5,
            1,
            0,
            0);

        std::cout
            << "[PASS] OPEN backend is skipped\n";
    }

    // ========================================================
    // TEST 5
    // OPEN -> HALF_OPEN -> CLOSED recovery
    // ========================================================

    {
        Backend &backend_a_state =
            load_balancer->backends()[0];

        // Verify A is OPEN before recovery.
        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::OPEN);

        // Wait until the open duration expires.
        std::this_thread::sleep_for(
            open_duration +
            std::chrono::milliseconds(50));

        // Backend A has recovered.
        backend_a.set_failing(false);

        // The next time A is selected, the circuit should
        // transition:
        //
        // OPEN -> HALF_OPEN
        //
        // The request acts as the probe.
        //
        // Successful response:
        //
        // HALF_OPEN -> CLOSED

        std::string response =
            send_request(gateway_port);

        assert(response == "backend A");

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::CLOSED);

        assert(
            backend_a_state.circuit_breaker.failure_count() == 0);

        assert_metric(
            *metrics,
            10,
            15,
            10,
            5,
            5,
            1,
            1,
            1);

        std::cout
            << "[PASS] HALF_OPEN probe successfully recovers backend\n";
    }

    // ========================================================
    // TEST 6
    // Recovered backend works normally
    // ========================================================

    {
        std::string response =
            send_request(gateway_port);

        // Both backends are healthy again.
        assert(
            response == "backend A" ||
            response == "backend B");

        Backend &backend_a_state =
            load_balancer->backends()[0];

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::CLOSED);

        assert_metric(
            *metrics,
            11,
            16,
            11,
            5,
            5,
            1,
            1,
            1);

        std::cout
            << "[PASS] recovered backend participates normally\n";
    }

    // ========================================================
    // Cleanup
    // ========================================================

    io_context.stop();

    if (io_thread.joinable())
    {
        io_thread.join();
    }

    std::cout
        << "\nAll gateway integration tests passed.\n";

    return 0;
}