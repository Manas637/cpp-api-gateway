#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <cstdint>
#include <mutex>

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

    void set_response_delay(
        std::chrono::milliseconds delay)
    {
        response_delay_ = delay;
    }

    std::string last_request_id() const
    {
        return last_request_id_;
    }

private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;

    std::string response_body_;

    std::atomic_bool failing_{false};
    std::chrono::milliseconds response_delay_{0};

    mutable std::mutex request_id_mutex_;
    std::string last_request_id_;

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

        auto response_timer =
            std::make_shared<asio::steady_timer>(
                io_context_);

        http::async_read(
            *socket_ptr,
            *buffer,
            *request,

            [this, socket_ptr, buffer, request, response_timer](
                beast::error_code ec,
                std::size_t)
            {
                if (ec)
                {
                    return;
                }

                auto request_id_it =
                    request->find("X-Request-ID");

                {
                    std::lock_guard<std::mutex> lock(
                        request_id_mutex_);

                    if (request_id_it != request->end())
                    {
                        last_request_id_ =
                            std::string(request_id_it->value());
                    }
                    else
                    {
                        last_request_id_.clear();
                    }
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

                if (response_delay_.count() > 0)
                {
                    response_timer->expires_after(
                        response_delay_);

                    response_timer->async_wait(
                        [socket_ptr, response, response_timer](
                            beast::error_code ec)
                        {
                            if (ec)
                            {
                                return;
                            }

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
                else
                {
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
                }
            });
    }
};

// ============================================================
// HTTP Client Helper
// ============================================================

struct HttpTestResponse
{
    unsigned int status;
    std::string body;
    std::string request_id;
};

HttpTestResponse send_request(
    unsigned short gateway_port,
    const std::string &request_id = "",
    unsigned int expected_status = 200)
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

    if (!request_id.empty())
    {
        request.set(
            "X-Request-ID",
            request_id);
    }

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

    HttpTestResponse result;

    result.status = response.result_int();
    result.body = response.body();

    auto response_request_id =
        response.find("X-Request-ID");

    if (response_request_id != response.end())
    {
        result.request_id =
            std::string(response_request_id->value());
    }

    return result;
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

    config.backend_response_timeout =
        std::chrono::milliseconds(100);

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
        auto response =
            send_request(gateway_port);

        assert(response.body == "backend A");

        response =
            send_request(gateway_port);

        assert(response.body == "backend B");

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
    // Request ID propagation
    // ========================================================

    {
        const std::string supplied_request_id =
            "test-request-123";

        auto response =
            send_request(
                gateway_port,
                supplied_request_id);

        assert(
            response.status == 200);

        assert(
            response.body == "backend B" ||
            response.body == "backend A");

        // Gateway preserves the client-provided ID.
        assert(
            response.request_id ==
            supplied_request_id);

        // The same ID must reach the backend.
        if (response.body == "backend A")
        {
            assert(
                backend_a.last_request_id() ==
                supplied_request_id);
        }
        else
        {
            assert(
                backend_b.last_request_id() ==
                supplied_request_id);
        }

        std::cout
            << "[PASS] supplied request ID propagation\n";
    }

    // ========================================================
    // TEST 3
    // Request ID generation
    // ========================================================

    {
        auto response =
            send_request(gateway_port);

        // Gateway must generate an ID when
        // the client does not provide one.
        assert(
            !response.request_id.empty());

        // The generated ID must reach the backend.
        if (response.body == "backend A")
        {
            assert(
                backend_a.last_request_id() ==
                response.request_id);
        }
        else
        {
            assert(
                response.body == "backend B");

            assert(
                backend_b.last_request_id() ==
                response.request_id);
        }

        std::cout
            << "[PASS] generated request ID propagation\n";
    }

    // ========================================================
    // TEST 4
    // Backend A failure -> failover to B
    // ========================================================

    backend_a.set_failing(true);

    {
        auto response =
            send_request(gateway_port);

        // A fails, gateway retries B.
        assert(response.body == "backend B");

        assert_metric(
            *metrics,
            5,
            6,
            5,
            1,
            1,
            0,
            0,
            0);

        std::cout
            << "[PASS] failover after backend failure\n";
    }

    // ========================================================
    // TEST 5
    // Repeated A failures -> circuit OPEN
    // ========================================================

    {
        for (std::size_t i = 0;
             i < failure_threshold;
             ++i)
        {
            auto response =
                send_request(gateway_port);

            // A fails and B handles the retry.
            assert(response.body == "backend B");
        }

        Backend &backend_a_state =
            load_balancer->backends()[0];

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::OPEN);

        assert_metric(
            *metrics,
            10,
            15,
            10,
            5,
            5,
            1,
            0,
            0);

        std::cout
            << "[PASS] circuit opens after repeated failures\n";
    }

    // ========================================================
    // TEST 6
    // OPEN backend is skipped
    // ========================================================

    {
        auto response =
            send_request(gateway_port);

        // A is OPEN, so load balancer skips it.
        assert(response.body == "backend B");

        Backend &backend_a_state =
            load_balancer->backends()[0];

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::OPEN);

        assert_metric(
            *metrics,
            11,
            16,
            11,
            5,
            5,
            1,
            0,
            0);

        std::cout
            << "[PASS] OPEN backend is skipped\n";
    }

    // ========================================================
    // TEST 7
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

        auto response =
            send_request(gateway_port);

        assert(response.body == "backend A");

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::CLOSED);

        assert(
            backend_a_state.circuit_breaker.failure_count() == 0);

        assert_metric(
            *metrics,
            12,
            17,
            12,
            5,
            5,
            1,
            1,
            1);

        std::cout
            << "[PASS] HALF_OPEN probe successfully recovers backend\n";
    }

    // ========================================================
    // TEST 8
    // Recovered backend works normally
    // ========================================================

    {
        auto response =
            send_request(gateway_port);

        // Both backends are healthy again.
        assert(
            response.body == "backend A" ||
            response.body == "backend B");

        Backend &backend_a_state =
            load_balancer->backends()[0];

        assert(
            backend_a_state.circuit_breaker.state() == CircuitBreaker::State::CLOSED);

        assert_metric(
            *metrics,
            13,
            18,
            13,
            5,
            5,
            1,
            1,
            1);

        std::cout
            << "[PASS] recovered backend participates normally\n";
    }

    // ============================================================
    // TEST 9
    // Backend response timeout -> failover
    // ============================================================

    backend_a.set_failing(false);
    backend_b.set_failing(false);

    backend_a.set_response_delay(
        std::chrono::milliseconds(500));

    backend_b.set_response_delay(
        std::chrono::milliseconds(0));

    {
        const auto backend_failures_before =
            metrics->backend_failures_total();

        const auto failovers_before =
            metrics->failovers_total();

        auto response =
            send_request(gateway_port);

        // Backend A exceeds the gateway's 100 ms
        // response timeout, so the gateway must fail over to B.
        assert(response.status == 200);
        assert(response.body == "backend B");

        assert(
            metrics->backend_failures_total() ==
            backend_failures_before + 1);

        assert(
            metrics->failovers_total() ==
            failovers_before + 1);

        std::cout
            << "[PASS] backend response timeout triggers failover\n";
    }

    backend_a.set_response_delay(
        std::chrono::milliseconds(0));

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