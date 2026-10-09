#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <cstdint>
#include <mutex>
#include <future>

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
    // The gateway runs on a dedicated io_context thread.
    // Wait briefly for the final metric updates before checking
    // the exact cumulative values.
    constexpr auto timeout = std::chrono::milliseconds(500);

    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        if (metrics.requests_total() == requests &&
            metrics.backend_requests_total() == backend_requests &&
            metrics.backend_successes_total() == backend_successes &&
            metrics.backend_failures_total() == backend_failures &&
            metrics.failovers_total() == failovers &&
            metrics.circuit_opens_total() == circuit_opens &&
            metrics.circuit_half_opens_total() == circuit_half_opens &&
            metrics.circuit_recoveries_total() == circuit_recoveries)
        {
            return;
        }

        std::this_thread::yield();
    }

    // If the expected values were not observed within the timeout,
    // fail with the original exact assertions.
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
// In-flight accounting helper
// ============================================================

// Poll until every backend reports the expected in-flight count.
// The gateway releases the slot in an async write callback, so a short
// poll avoids racing the client response.
void wait_for_in_flight(
    const LoadBalancer &load_balancer,
    std::size_t expected,
    std::chrono::milliseconds timeout =
        std::chrono::milliseconds(1000))
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline)
    {
        bool all_match = true;

        for (const auto &backend : load_balancer.backends())
        {
            if (backend.in_flight_requests.load() != expected)
            {
                all_match = false;
                break;
            }
        }

        if (all_match)
        {
            return;
        }

        std::this_thread::yield();
    }

    for (const auto &backend : load_balancer.backends())
    {
        assert(backend.in_flight_requests.load() == expected);
    }
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
    // Maximum one retry when both backends fail
    // ========================================================

    backend_a.set_failing(true);
    backend_b.set_failing(true);

    {
        const auto backend_requests_before =
            metrics->backend_requests_total();

        const auto backend_failures_before =
            metrics->backend_failures_total();

        const auto failovers_before =
            metrics->failovers_total();

        auto response =
            send_request(
                gateway_port,
                "",
                503);

        // First backend fails, gateway retries once on
        // the second backend, which also fails.
        assert(response.status == 503);
        assert(response.body == "Service Unavailable");

        // Exactly two backend attempts:
        // A + one retry on B.
        assert(
            metrics->backend_requests_total() ==
            backend_requests_before + 2);

        // Both backend attempts failed.
        assert(
            metrics->backend_failures_total() ==
            backend_failures_before + 2);

        // Exactly one failover occurred.
        assert(
            metrics->failovers_total() ==
            failovers_before + 1);

        std::cout
            << "[PASS] maximum one retry when both backends fail\n";
    }

    backend_a.set_failing(false);
    backend_b.set_failing(false);

    // ========================================================
    // TEST 5
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
            6, // requests
            8, // backend_requests
            5, // backend_successes
            3, // backend_failures
            2, // failovers
            0, // circuit_opens
            0, // circuit_half_opens
            0  // circuit_recoveries
        );

        std::cout
            << "[PASS] failover after backend failure\n";
    }

    // ========================================================
    // TEST 6
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
            11,
            16,
            10,
            6,
            5,
            1,
            0,
            0);

        std::cout
            << "[PASS] circuit opens after repeated failures\n";
    }

    // ========================================================
    // TEST 7
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
            12,
            17,
            11,
            6,
            5,
            1,
            0,
            0);

        std::cout
            << "[PASS] OPEN backend is skipped\n";
    }

    // ========================================================
    // TEST 8
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
            13,
            18,
            12,
            6,
            5,
            1,
            1,
            1);

        std::cout
            << "[PASS] HALF_OPEN probe successfully recovers backend\n";
    }

    // ========================================================
    // TEST 9
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
            14,
            19,
            13,
            6,
            5,
            1,
            1,
            1);

        std::cout
            << "[PASS] recovered backend participates normally\n";
    }

    // ============================================================
    // TEST 10
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

        const auto backend_requests_before =
            metrics->backend_requests_total();

        const auto backend_successes_before =
            metrics->backend_successes_total();

        auto response =
            send_request(gateway_port);

        // Backend A exceeds the gateway's 100 ms
        // response timeout, so the gateway must fail over to B.
        assert(response.status == 200);
        assert(response.body == "backend B");

        // Exactly two backend attempts: A times out, then B succeeds.
        assert(
            metrics->backend_requests_total() ==
            backend_requests_before + 2);

        // Exactly one backend attempt succeeds.
        assert(
            metrics->backend_successes_total() ==
            backend_successes_before + 1);

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

    // ============================================================
    // TEST 11
    // Retry must not select the failed backend again
    // when no alternative backend is eligible.
    // ============================================================

    {
        // Change backend health on the gateway's I/O thread
        // to avoid racing with LoadBalancer::next().
        auto set_backend_healthy =
            [&](std::size_t index, bool healthy)
        {
            std::promise<void> updated;
            auto done = updated.get_future();

            asio::post(
                io_context,
                [&load_balancer, &updated, index, healthy]()
                {
                    load_balancer->backends()[index].healthy =
                        healthy;

                    updated.set_value();
                });

            done.wait();
        };

        // Ensure A is the only eligible backend, regardless
        // of the current round-robin index.
        set_backend_healthy(0, true);
        set_backend_healthy(1, false);

        backend_a.set_failing(true);

        const auto backend_requests_before =
            metrics->backend_requests_total();

        const auto backend_failures_before =
            metrics->backend_failures_total();

        const auto failovers_before =
            metrics->failovers_total();

        auto response = send_request(gateway_port, "", 503);

        assert(response.status == 503);
        assert(response.body == "Service Unavailable");

        // A was attempted once. The retry must exclude A,
        // discover that B is ineligible, and return 503.
        assert(
            metrics->backend_requests_total() ==
            backend_requests_before + 1);

        assert(
            metrics->backend_failures_total() ==
            backend_failures_before + 1);

        // No alternative backend was selected, so no
        // successful backend switch was recorded.
        assert(
            metrics->failovers_total() ==
            failovers_before);

        // Restore shared state for clean shutdown.
        backend_a.set_failing(false);
        set_backend_healthy(1, true);

        std::cout
            << "[PASS] retry excludes failed backend when "
               "no alternative is eligible\n";
    }

    // ========================================================
    // In-flight accounting test helpers
    // ========================================================

    // Mutate shared LoadBalancer state on the I/O thread to avoid
    // racing with LoadBalancer::next().
    auto set_backend_healthy =
        [&](std::size_t index, bool healthy)
    {
        std::promise<void> updated;
        auto done = updated.get_future();

        asio::post(
            io_context,
            [&load_balancer, &updated, index, healthy]()
            {
                load_balancer->backends()[index].healthy = healthy;
                updated.set_value();
            });

        done.wait();
    };

    auto set_strategy =
        [&](LoadBalancingStrategy strategy)
    {
        std::promise<void> updated;
        auto done = updated.get_future();

        asio::post(
            io_context,
            [&load_balancer, &updated, strategy]()
            {
                load_balancer->set_strategy(strategy);
                updated.set_value();
            });

        done.wait();
    };

    auto wait_for_backend_in_flight =
        [&](std::size_t index, std::size_t expected)
    {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(1000);

        while (load_balancer->backends()[index]
                       .in_flight_requests.load() != expected &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }

        assert(
            load_balancer->backends()[index]
                    .in_flight_requests.load() == expected);
    };

    // ========================================================
    // TEST 12
    // In-flight count is acquired during a backend request and
    // released once the response completes.
    // ========================================================

    {
        set_backend_healthy(0, true);
        set_backend_healthy(1, false);

        backend_a.set_failing(false);
        backend_b.set_failing(false);

        backend_a.set_response_delay(
            std::chrono::milliseconds(70));

        backend_b.set_response_delay(
            std::chrono::milliseconds(0));

        HttpTestResponse response;

        std::thread request_thread(
            [&]()
            {
                response = send_request(gateway_port);
            });

        // Backend A is the only eligible backend, so the request must
        // be counted against A while its delayed response is pending.
        wait_for_backend_in_flight(0, 1);

        assert(
            load_balancer->backends()[1]
                    .in_flight_requests.load() == 0);

        request_thread.join();

        assert(response.status == 200);
        assert(response.body == "backend A");

        wait_for_in_flight(*load_balancer, 0);

        backend_a.set_response_delay(
            std::chrono::milliseconds(0));

        set_backend_healthy(1, true);

        std::cout
            << "[PASS] in-flight request acquired and released\n";
    }

    // ========================================================
    // TEST 13
    // Timeout + retry releases the failed attempt and counts the
    // retry attempt correctly.
    // ========================================================

    {
        set_backend_healthy(0, true);
        set_backend_healthy(1, false);

        backend_a.set_failing(false);
        backend_b.set_failing(false);

        backend_a.set_response_delay(
            std::chrono::milliseconds(500));

        backend_b.set_response_delay(
            std::chrono::milliseconds(0));

        const auto failures_before =
            metrics->backend_failures_total();

        const auto successes_before =
            metrics->backend_successes_total();

        const auto failovers_before =
            metrics->failovers_total();

        HttpTestResponse response;

        std::thread request_thread(
            [&]()
            {
                response = send_request(gateway_port);
            });

        wait_for_backend_in_flight(0, 1);

        // Make the alternative backend eligible so the timed-out
        // attempt can retry onto it.
        set_backend_healthy(1, true);

        request_thread.join();

        assert(response.status == 200);
        assert(response.body == "backend B");

        wait_for_in_flight(*load_balancer, 0);

        assert(
            metrics->backend_failures_total() ==
            failures_before + 1);

        assert(
            metrics->backend_successes_total() ==
            successes_before + 1);

        assert(
            metrics->failovers_total() ==
            failovers_before + 1);

        backend_a.set_response_delay(
            std::chrono::milliseconds(0));

        std::cout
            << "[PASS] timeout + retry releases failed attempt\n";
    }

    // ========================================================
    // TEST 14
    // Retry exhaustion releases the slot (no leak).
    // ========================================================

    {
        set_backend_healthy(0, true);
        set_backend_healthy(1, false);

        backend_a.set_failing(true);
        backend_b.set_failing(false);

        auto response =
            send_request(gateway_port, "", 503);

        assert(response.status == 503);
        assert(response.body == "Service Unavailable");

        wait_for_in_flight(*load_balancer, 0);

        backend_a.set_failing(false);
        set_backend_healthy(1, true);

        std::cout
            << "[PASS] no in-flight leak after retry exhaustion\n";
    }

    // ========================================================
    // TEST 15
    // No leak or underflow across many sequential requests.
    // ========================================================

    {
        set_backend_healthy(0, true);
        set_backend_healthy(1, true);

        backend_a.set_failing(false);
        backend_b.set_failing(false);

        backend_a.set_response_delay(
            std::chrono::milliseconds(0));

        backend_b.set_response_delay(
            std::chrono::milliseconds(0));

        for (int i = 0; i < 20; ++i)
        {
            auto response = send_request(gateway_port);
            assert(response.status == 200);
        }

        wait_for_in_flight(*load_balancer, 0);

        // An underflow would wrap std::size_t to a huge value and
        // fail this assertion.
        for (const auto &backend : load_balancer->backends())
        {
            assert(backend.in_flight_requests.load() == 0);
        }

        std::cout
            << "[PASS] no in-flight leak or underflow over 20 requests\n";
    }

    // ========================================================
    // TEST 16
    // Concurrent least-connections: while one backend is busy the
    // second request is routed to the idle backend; both counts are
    // tracked independently.
    // ========================================================

    {
        set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

        set_backend_healthy(0, true);
        set_backend_healthy(1, false);

        backend_a.set_failing(false);
        backend_b.set_failing(false);

        // Both responses stay comfortably below the 100 ms response
        // timeout while leaving a window to observe backend A busy.
        backend_a.set_response_delay(
            std::chrono::milliseconds(60));

        backend_b.set_response_delay(
            std::chrono::milliseconds(30));

        HttpTestResponse first_response;

        std::thread first_thread(
            [&]()
            {
                first_response = send_request(gateway_port);
            });

        // Wait until the first request owns backend A.
        wait_for_backend_in_flight(0, 1);

        // Backend A is still busy when the second request is
        // dispatched.
        assert(
            load_balancer->backends()[0]
                    .in_flight_requests.load() == 1);

        // Make backend B eligible; the second request must prefer the
        // idle backend B over the busy backend A.
        set_backend_healthy(1, true);

        auto second_response = send_request(gateway_port);

        first_thread.join();

        assert(first_response.status == 200);
        assert(second_response.status == 200);
        assert(first_response.body == "backend A");
        assert(second_response.body == "backend B");

        wait_for_in_flight(*load_balancer, 0);

        backend_a.set_response_delay(
            std::chrono::milliseconds(0));

        backend_b.set_response_delay(
            std::chrono::milliseconds(0));

        set_strategy(LoadBalancingStrategy::ROUND_ROBIN);

        std::cout
            << "[PASS] concurrent least-connections picks idle backend\n";
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