#include <atomic>
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
#include "health_checker.hpp"
#include "in_memory_rate_limit_store.hpp"
#include "load_balancer.hpp"
#include "metrics.hpp"
#include "rate_limiter.hpp"
#include "server.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

using namespace std::chrono_literals;

// ============================================================
// Responding Backend
//
// Accepts gateway connections and answers each request with a 200
// after an optional delay, then closes the connection.
// ============================================================

class RespondBackend
{
public:
    RespondBackend(
        asio::io_context &io_context,
        unsigned short port)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), port))
    {
        accept();
    }

    void set_response_delay(std::chrono::milliseconds delay)
    {
        response_delay_ = delay;
    }

private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;
    std::chrono::milliseconds response_delay_{0};

    void accept()
    {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    handle(std::move(socket));
                }

                if (acceptor_.is_open())
                {
                    accept();
                }
            });
    }

    void handle(tcp::socket socket)
    {
        auto socket_ptr =
            std::make_shared<tcp::socket>(std::move(socket));
        auto buffer = std::make_shared<beast::flat_buffer>();
        auto request = std::make_shared<
            http::request<http::string_body>>();
        auto timer =
            std::make_shared<asio::steady_timer>(socket_ptr->get_executor());

        http::async_read(
            *socket_ptr,
            *buffer,
            *request,
            [this, socket_ptr, buffer, request, timer](
                beast::error_code ec,
                std::size_t)
            {
                if (ec)
                {
                    return;
                }

                auto send = [socket_ptr]()
                {
                    auto response = std::make_shared<
                        http::response<http::string_body>>(
                        http::status::ok,
                        11);

                    response->set(
                        http::field::content_type,
                        "text/plain");
                    response->body() = "ok";
                    response->prepare_payload();

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
                            socket_ptr->close(shutdown_ec);
                        });
                };

                if (response_delay_.count() > 0)
                {
                    timer->expires_after(response_delay_);
                    timer->async_wait(
                        [timer, send](beast::error_code ec)
                        {
                            if (!ec)
                            {
                                send();
                            }
                        });
                }
                else
                {
                    send();
                }
            });
    }
};

// ============================================================
// Silent Backend
//
// Accepts a backend connection and never responds, keeping the
// gateway's request permanently in flight until it is force-closed.
// ============================================================

class SilentBackend
{
public:
    SilentBackend(asio::io_context &io_context, unsigned short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
    {
        accept();
    }

private:
    tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<tcp::socket>> connections_;

    void accept()
    {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    connections_.push_back(
                        std::make_shared<tcp::socket>(std::move(socket)));
                }

                if (acceptor_.is_open())
                {
                    accept();
                }
            });
    }
};

// ============================================================
// Client helpers
// ============================================================

struct RawResponse
{
    unsigned int status = 0;
    bool connection_close = false;
};

// Outcome of a bounded HTTP read. `timed_out` distinguishes a genuine
// peer close/EOF from a deadline expiry, so a test can fail instead of
// blocking when the expected event never happens.
struct ReadResult
{
    RawResponse response;
    bool timed_out = false;
    bool errored = false;
};

void send_get(tcp::socket &socket)
{
    http::request<http::string_body> request{http::verb::get, "/", 11};
    request.set(http::field::host, "localhost");

    http::write(socket, request);
}

// Reads one HTTP response with a bounded wait. The read and the deadline
// timer run on the same io_context, and therefore the same thread, so
// the timeout and the socket operation can never race. On timeout the
// socket is cancelled and `timed_out` is reported instead of blocking
// forever.
ReadResult read_response(
    asio::io_context &io_context,
    tcp::socket &socket,
    std::chrono::milliseconds timeout)
{
    io_context.restart();

    ReadResult result;

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    asio::steady_timer timer(io_context);

    bool read_done = false;

    timer.expires_after(timeout);

    timer.async_wait(
        [&](beast::error_code ec)
        {
            if (!ec && !read_done)
            {
                result.timed_out = true;

                beast::error_code ignored;
                socket.cancel(ignored);
            }
        });

    http::async_read(
        socket,
        buffer,
        response,
        [&](beast::error_code ec, std::size_t)
        {
            read_done = true;
            timer.cancel();

            if (ec)
            {
                result.errored = true;
                return;
            }

            result.response.status = response.result_int();

            auto connection = response.find(http::field::connection);

            if (connection != response.end() &&
                beast::iequals(connection->value(), "close"))
            {
                result.response.connection_close = true;
            }
        });

    io_context.run();

    return result;
}

bool wait_for_active_connections(Metrics &metrics, std::uint64_t expected)
{
    for (int i = 0; i < 200; ++i)
    {
        if (metrics.active_connections() == expected)
        {
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return metrics.active_connections() == expected;
}

std::size_t backend_in_flight(LoadBalancer &load_balancer)
{
    return load_balancer.backends()
        .front()
        .in_flight_requests.load(std::memory_order_relaxed);
}

// True when the peer has closed the connection: a further read fails
// (clean EOF or reset) instead of yielding another HTTP message. Bounded
// so a missing close fails the test rather than hanging indefinitely.
bool connection_closed(
    asio::io_context &io_context,
    tcp::socket &socket,
    std::chrono::milliseconds timeout)
{
    auto result = read_response(io_context, socket, timeout);

    return result.errored && !result.timed_out;
}

// ============================================================
// TEST 1: An idle keep-alive connection is answered 503 during
// drain, then the server stops before the grace period elapses.
// ============================================================

void test_idle_drain_returns_503()
{
    constexpr unsigned short gateway_port = 18090;
    constexpr unsigned short backend_port = 19010;

    asio::io_context io_context;

    RespondBackend backend(io_context, backend_port);

    auto load_balancer = std::make_shared<LoadBalancer>(
        std::vector<Backend>{Backend("127.0.0.1", backend_port)});

    auto metrics = std::make_shared<Metrics>();

    auto rate_limit_store = std::make_shared<InMemoryRateLimitStore>();

    auto rate_limiter = std::make_shared<RateLimiter>(
        *rate_limit_store, 1000.0, 1000.0);

    GatewayConfig config;
    config.port = gateway_port;
    config.backends = {Backend("127.0.0.1", backend_port)};
    config.rate_limit_capacity = 1000.0;
    config.rate_limit_refill_rate = 1000.0;
    config.shutdown_grace_period = std::chrono::milliseconds(2000);

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        false);

    std::thread io_thread([&io_context]() { io_context.run(); });

    asio::io_context client_context;
    tcp::socket client(client_context);
    asio::connect(
        client,
        tcp::resolver(client_context).resolve(
            "127.0.0.1", std::to_string(gateway_port)));

    assert(wait_for_active_connections(*metrics, 1));

    const auto started = std::chrono::steady_clock::now();

    // Begin draining and wait until it has actually run before sending
    // the request, so the request deterministically hits the drain path
    // rather than relying on a sleep.
    std::atomic<bool> draining{false};

    asio::post(
        io_context,
        [&server, &draining]()
        {
            server.begin_shutdown();
            draining.store(true);
        });

    for (int i = 0; i < 200 && !draining.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    assert(draining.load());

    // The keep-alive connection is idle: the next request must be
    // answered with 503 + Connection: close.
    send_get(client);

    auto response =
        read_response(client_context, client, 2000ms).response;

    assert(response.status == 503);
    assert(response.connection_close);
    assert(connection_closed(client_context, client, 2000ms));

    io_thread.join();

    const auto elapsed = std::chrono::steady_clock::now() - started;

    assert(elapsed < std::chrono::milliseconds(1500));

    assert(wait_for_active_connections(*metrics, 0));

    std::cout
        << "[PASS] idle keep-alive drained with 503 during shutdown\n";
}

// ============================================================
// TEST 2: An in-flight request completes normally during drain,
// and the server stops well before the grace period elapses.
// ============================================================

void test_in_flight_request_completes()
{
    constexpr unsigned short gateway_port = 18091;
    constexpr unsigned short backend_port = 19011;

    asio::io_context io_context;

    RespondBackend backend(io_context, backend_port);
    backend.set_response_delay(std::chrono::milliseconds(500));

    auto load_balancer = std::make_shared<LoadBalancer>(
        std::vector<Backend>{Backend("127.0.0.1", backend_port)});

    auto metrics = std::make_shared<Metrics>();

    auto rate_limit_store = std::make_shared<InMemoryRateLimitStore>();

    auto rate_limiter = std::make_shared<RateLimiter>(
        *rate_limit_store, 1000.0, 1000.0);

    GatewayConfig config;
    config.port = gateway_port;
    config.backends = {Backend("127.0.0.1", backend_port)};
    config.rate_limit_capacity = 1000.0;
    config.rate_limit_refill_rate = 1000.0;
    config.backend_response_timeout = std::chrono::milliseconds(2000);
    config.request_timeout = std::chrono::milliseconds(2000);
    config.shutdown_grace_period = std::chrono::milliseconds(3000);

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        false);

    std::thread io_thread([&io_context]() { io_context.run(); });

    asio::io_context client_context;
    tcp::socket client(client_context);
    asio::connect(
        client,
        tcp::resolver(client_context).resolve(
            "127.0.0.1", std::to_string(gateway_port)));

    assert(wait_for_active_connections(*metrics, 1));

    send_get(client);

    // Wait until the request is past the drain check and being
    // forwarded to the backend, so shutdown races it deterministically.
    for (int i = 0; i < 200 && metrics->requests_total() == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(metrics->requests_total() == 1);

    const auto started = std::chrono::steady_clock::now();

    asio::post(io_context, [&server]() { server.begin_shutdown(); });

    auto response =
        read_response(client_context, client, 2000ms).response;

    assert(response.status == 200);

    // The final response during drain must close the connection.
    assert(response.connection_close);

    io_thread.join();

    const auto elapsed = std::chrono::steady_clock::now() - started;

    assert(elapsed < std::chrono::milliseconds(2500));

    assert(metrics->active_connections() == 0);

    assert(connection_closed(client_context, client, 2000ms));

    assert(backend_in_flight(*load_balancer) == 0);

    std::cout
        << "[PASS] in-flight request completes during graceful shutdown\n";
}

// ============================================================
// TEST 3: A request that never completes is force-closed when the
// grace period elapses.
// ============================================================

void test_grace_period_force_closes()
{
    constexpr unsigned short gateway_port = 18092;
    constexpr unsigned short backend_port = 19012;

    asio::io_context io_context;

    SilentBackend backend(io_context, backend_port);

    auto load_balancer = std::make_shared<LoadBalancer>(
        std::vector<Backend>{Backend("127.0.0.1", backend_port)});

    auto metrics = std::make_shared<Metrics>();

    auto rate_limit_store = std::make_shared<InMemoryRateLimitStore>();

    auto rate_limiter = std::make_shared<RateLimiter>(
        *rate_limit_store, 1000.0, 1000.0);

    GatewayConfig config;
    config.port = gateway_port;
    config.backends = {Backend("127.0.0.1", backend_port)};
    config.rate_limit_capacity = 1000.0;
    config.rate_limit_refill_rate = 1000.0;
    config.backend_response_timeout = std::chrono::milliseconds(30000);
    config.request_timeout = std::chrono::milliseconds(30000);
    config.shutdown_grace_period = std::chrono::milliseconds(300);

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        false);

    std::thread io_thread([&io_context]() { io_context.run(); });

    asio::io_context client_context;
    tcp::socket client(client_context);
    asio::connect(
        client,
        tcp::resolver(client_context).resolve(
            "127.0.0.1", std::to_string(gateway_port)));

    assert(wait_for_active_connections(*metrics, 1));

    send_get(client);

    // Ensure the request is already in flight (past the drain check)
    // before shutdown begins, so the grace timer, not the 503 path, is
    // what eventually ends the connection.
    for (int i = 0; i < 200 && metrics->requests_total() == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    assert(metrics->requests_total() == 1);

    const auto started = std::chrono::steady_clock::now();

    asio::post(io_context, [&server]() { server.begin_shutdown(); });

    // The gateway never answers; the only way this unblocks is the
    // forced close at the end of the grace period.
    auto bounded = read_response(client_context, client, 1500ms);

    assert(!bounded.timed_out);

    auto response = bounded.response;

    assert(response.status == 0);

    io_thread.join();

    const auto elapsed = std::chrono::steady_clock::now() - started;

    assert(elapsed >= std::chrono::milliseconds(250));
    assert(elapsed < std::chrono::milliseconds(2000));

    assert(metrics->active_connections() == 0);

    assert(backend_in_flight(*load_balancer) == 0);

    std::cout
        << "[PASS] grace period expiry force-closes hung connection\n";
}

// ============================================================
// TEST 4: Repeated shutdown initiation is idempotent and safe.
// ============================================================

void test_repeated_shutdown_is_safe()
{
    constexpr unsigned short gateway_port = 18093;
    constexpr unsigned short backend_port = 19013;

    asio::io_context io_context;

    SilentBackend backend(io_context, backend_port);

    auto load_balancer = std::make_shared<LoadBalancer>(
        std::vector<Backend>{Backend("127.0.0.1", backend_port)});

    auto metrics = std::make_shared<Metrics>();

    auto rate_limit_store = std::make_shared<InMemoryRateLimitStore>();

    auto rate_limiter = std::make_shared<RateLimiter>(
        *rate_limit_store, 1000.0, 1000.0);

    GatewayConfig config;
    config.port = gateway_port;
    config.backends = {Backend("127.0.0.1", backend_port)};
    config.rate_limit_capacity = 1000.0;
    config.rate_limit_refill_rate = 1000.0;
    config.shutdown_grace_period = std::chrono::milliseconds(1000);

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        false);

    std::atomic<bool> io_finished{false};

    std::thread io_thread(
        [&io_context, &io_finished]()
        {
            io_context.run();
            io_finished.store(true);
        });

    asio::io_context client_context;
    tcp::socket client(client_context);
    asio::connect(
        client,
        tcp::resolver(client_context).resolve(
            "127.0.0.1", std::to_string(gateway_port)));

    assert(wait_for_active_connections(*metrics, 1));

    // Both graceful and forced initiation are called twice; the second
    // of each must be a safe no-op.
    std::atomic<bool> shutdowns_applied{false};

    asio::post(
        io_context,
        [&server, &shutdowns_applied]()
        {
            server.begin_shutdown();
            server.begin_shutdown();
            server.force_shutdown();
            server.force_shutdown();
            shutdowns_applied.store(true);
        });

    io_thread.join();

    assert(io_finished.load());
    assert(shutdowns_applied.load());
    assert(metrics->active_connections() == 0);
    assert(backend_in_flight(*load_balancer) == 0);

    std::cout
        << "[PASS] repeated shutdown initiation is idempotent\n";
}

// ============================================================
// TEST 5: ShutdownCoordinator with no live sessions finishes
// immediately instead of waiting out the grace period.
// ============================================================

void test_zero_session_fast_path()
{
    asio::io_context io_context;

    auto coordinator = std::make_shared<ShutdownCoordinator>(
        io_context,
        std::chrono::milliseconds(10000));

    bool force_close_called = false;

    coordinator->set_force_close(
        [&force_close_called]() { force_close_called = true; });

    coordinator->begin();

    // No sessions: the io_context must be stopped synchronously.
    assert(io_context.stopped());

    io_context.run();

    assert(!force_close_called);

    std::cout
        << "[PASS] zero-session shutdown finishes immediately\n";
}

// ============================================================
// TEST 6: HealthChecker::stop() cancels the recurring schedule.
// ============================================================

void test_health_checker_stop()
{
    asio::io_context io_context;

    // Unreachable port: connection attempts fail promptly.
    auto load_balancer = std::make_shared<LoadBalancer>(
        std::vector<Backend>{Backend("127.0.0.1", 19099)});

    auto health_checker =
        std::make_shared<HealthChecker>(io_context, load_balancer);

    health_checker->start();

    // stop() runs on the io thread and cancels the recurring check
    // timer. Once it and the in-flight check settle, no work remains
    // and run() must return promptly. Had stop() failed, the 5s
    // recurring timer would keep run() alive indefinitely.
    asio::post(io_context, [health_checker]() { health_checker->stop(); });

    std::atomic<bool> io_finished{false};

    std::thread io_thread(
        [&io_context, &io_finished]()
        {
            io_context.run();
            io_finished.store(true);
        });

    for (int i = 0; i < 200 && !io_finished.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    bool stopped_in_time = io_finished.load();

    if (!stopped_in_time)
    {
        io_context.stop();
    }

    io_thread.join();

    assert(stopped_in_time);

    std::cout
        << "[PASS] health checker stop cancels scheduling\n";
}

int main()
{
    test_idle_drain_returns_503();
    test_in_flight_request_completes();
    test_grace_period_force_closes();
    test_repeated_shutdown_is_safe();
    test_zero_session_fast_path();
    test_health_checker_stop();

    std::cout << "All graceful shutdown tests passed.\n";

    return 0;
}
