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
// Silent Backend
//
// Accepts the gateway's backend connection and then never reads
// from or responds to it, so the gateway keeps the backend attempt
// in flight until it is torn down.
// ============================================================

class SilentBackend
{
public:
    SilentBackend(
        asio::io_context &io_context,
        unsigned short port)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), port))
    {
        accept();
    }

    bool accepted() const
    {
        return accepted_.load();
    }

private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;

    // Connections must be kept alive: only destroying them (at scope
    // exit) makes the gateway's pending read observe a closed socket.
    std::vector<std::shared_ptr<tcp::socket>> connections_;

    std::atomic_bool accepted_{false};

    void accept()
    {
        acceptor_.async_accept(
            [this](
                beast::error_code ec,
                tcp::socket socket)
            {
                if (!ec)
                {
                    connections_.push_back(
                        std::make_shared<tcp::socket>(
                            std::move(socket)));

                    accepted_.store(true);
                }

                if (acceptor_.is_open())
                {
                    accept();
                }
            });
    }
};

// ============================================================
// Teardown Accounting Test
// ============================================================

int main()
{
    constexpr unsigned short gateway_port = 18082;
    constexpr unsigned short backend_port = 19004;

    std::shared_ptr<LoadBalancer> load_balancer;

    auto metrics = std::make_shared<Metrics>();

    auto rate_limit_store =
        std::make_shared<InMemoryRateLimitStore>();

    auto rate_limiter =
        std::make_shared<RateLimiter>(
            *rate_limit_store,
            1000.0,
            1000.0);

    // Keep the accounting state reachable after the io_context is
    // destroyed, so the released counter can be asserted afterwards.
    std::vector<Backend> backends;

    backends.emplace_back(
        "127.0.0.1",
        backend_port);

    load_balancer =
        std::make_shared<LoadBalancer>(
            std::move(backends));

    {
        asio::io_context io_context;

        // 30 second backend timeout: long enough that the response
        // timer never fires while the attempt is deliberately left in
        // flight below.
        GatewayConfig config;

        config.port = gateway_port;

        config.backends = {
            Backend(
                "127.0.0.1",
                backend_port)};

        config.rate_limit_capacity = 1000.0;

        config.rate_limit_refill_rate = 1000.0;

        config.backend_response_timeout =
            std::chrono::seconds(30);

        SilentBackend backend(
            io_context,
            backend_port);

        // The server starts accepting connections in its constructor.
        Server server(
            io_context,
            config,
            load_balancer,
            rate_limiter,
            metrics,
            false);

        std::thread io_thread(
            [&io_context]()
            {
                io_context.run();
            });

        // Give the server/backend acceptors time to start.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));

        tcp::socket client(io_context);

        auto endpoints =
            tcp::resolver(io_context).resolve(
                "127.0.0.1",
                std::to_string(gateway_port));

        asio::connect(client, endpoints);

        http::request<http::string_body> request{
            http::verb::get,
            "/",
            11};

        request.set(http::field::host, "localhost");
        request.set(http::field::connection, "close");

        http::write(client, request);

        // Wait until the gateway has assigned the request to the
        // backend and the backend has accepted the connection, so the
        // attempt is genuinely in flight and still holds a slot.
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(3000);

        while (
            !backend.accepted() ||
            load_balancer->backends()[0]
                    .in_flight_requests.load() != 1)
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }

            std::this_thread::yield();
        }

        assert(backend.accepted());

        assert(
            load_balancer->backends()[0]
                    .in_flight_requests.load() == 1);

        // Stop the I/O thread while the attempt is still pending, so
        // the slot can never be released on a normal completion path.
        io_context.stop();

        io_thread.join();

        // No further work is needed on the client socket; it is closed
        // when it goes out of scope at the end of this block.
    }

    // Destroying io_context dropped every queued handler closure,
    // which released the last Session shared_ptr. The Session
    // destructor must return the still-held backend slot instead of
    // leaking it, even though the attempt never completed.
    assert(
        load_balancer->backends().front()
                .in_flight_requests.load() == 0);

    std::cout
        << "[PASS] session teardown releases the in-flight backend slot\n";

    return 0;
}