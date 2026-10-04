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

int main()
{
    constexpr unsigned short gateway_port = 18083;
    constexpr unsigned short backend_port = 19005;

    asio::io_context io_context;

    // -------------------------------------------------------------------------
    // Backend
    // -------------------------------------------------------------------------

    auto backend_acceptor =
        std::make_shared<tcp::acceptor>(
            io_context,
            tcp::endpoint(tcp::v4(), backend_port));

    auto accept_backend =
        std::make_shared<std::function<void()>>();

    *accept_backend = [backend_acceptor, accept_backend]()
    {
        backend_acceptor->async_accept(
            [backend_acceptor, accept_backend](
                beast::error_code ec,
                tcp::socket socket)
            {
                if (ec)
                {
                    if (ec != asio::error::operation_aborted)
                    {
                        (*accept_backend)();
                    }

                    return;
                }

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
                        response->keep_alive(false);
                        response->prepare_payload();

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

                (*accept_backend)();
            });
    };

    (*accept_backend)();

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
            100.0,
            100.0);

    GatewayConfig config;
    config.port = gateway_port;

    config.backends = {
        Backend(
            "127.0.0.1",
            backend_port,
            5,
            std::chrono::milliseconds(100))};

    config.rate_limit_capacity = 100.0;
    config.rate_limit_refill_rate = 100.0;

    Server server(
        io_context,
        config,
        load_balancer,
        rate_limiter,
        metrics,
        false);

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
    // TEST 1: Connection is counted after being accepted
    // -------------------------------------------------------------------------

    asio::io_context client_io_context;

    tcp::socket client_socket(client_io_context);

    tcp::resolver resolver(client_io_context);

    auto endpoints =
        resolver.resolve(
            "127.0.0.1",
            std::to_string(gateway_port));

    asio::connect(
        client_socket,
        endpoints);

    // The server accepts asynchronously, so give the gateway's event loop
    // time to process the connection.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(50));

    assert(metrics->active_connections() == 1);

    std::cout
        << "[PASS] active connection is counted\n";

    // -------------------------------------------------------------------------
    // TEST 2: Connection remains counted while the socket is alive
    // -------------------------------------------------------------------------

    std::this_thread::sleep_for(
        std::chrono::milliseconds(50));

    assert(metrics->active_connections() == 1);

    std::cout
        << "[PASS] active connection remains counted\n";

    // -------------------------------------------------------------------------
    // TEST 3: Closing the client connection decrements the metric
    // -------------------------------------------------------------------------

    beast::error_code shutdown_ec;

    client_socket.shutdown(
        tcp::socket::shutdown_both,
        shutdown_ec);

    client_socket.close(shutdown_ec);

    // The gateway must process the client's EOF and destroy the Session.
    for (int i = 0;
         i < 100 && metrics->active_connections() != 0;
         ++i)
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(10));
    }

    assert(metrics->active_connections() == 0);

    std::cout
        << "[PASS] closed connection is removed from active count\n";

    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------

    beast::error_code acceptor_ec;

    backend_acceptor->close(acceptor_ec);

    io_context.stop();

    if (io_thread.joinable())
    {
        io_thread.join();
    }

    std::cout
        << "\nAll active connection integration tests passed.\n";

    return 0;
}