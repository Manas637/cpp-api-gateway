#include <iostream>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include "load_balancer.hpp"
#include "health_checker.hpp"
#include "rate_limiter.hpp"
#include "redis_rate_limit_store.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session>
{
private:
    tcp::socket socket_;
    beast::flat_buffer buffer_;

    http::request<http::string_body> request_;

    tcp::resolver resolver_;
    tcp::socket backend_socket_;

    beast::flat_buffer backend_buffer_;
    http::response<http::string_body> backend_response_;

    // Response sent when rate limit is exceeded.
    http::response<http::string_body> rate_limit_response_;

    std::shared_ptr<LoadBalancer> load_balancer_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    bool rate_limiting_enabled_;

    Backend *selected_backend_ = nullptr;
    bool backend_connected_ = false;

    std::string client_id_;

public:
    Session(
        tcp::socket socket,
        std::shared_ptr<LoadBalancer> load_balancer,
        std::shared_ptr<RateLimiter> rate_limiter,
        bool rate_limiting_enabled)
        : socket_(std::move(socket)),
          resolver_(socket_.get_executor()),
          backend_socket_(socket_.get_executor()),
          load_balancer_(std::move(load_balancer)),
          rate_limiter_(std::move(rate_limiter)),
          rate_limiting_enabled_(rate_limiting_enabled)
    {
    }

    void start()
    {
        selected_backend_ = &load_balancer_->next();
        read_request();
    }

private:
    // ============================================================
    // Client -> Gateway
    // ============================================================

    void read_request()
    {
        auto self = shared_from_this();

        http::async_read(
            socket_,
            buffer_,
            request_,
            [self](beast::error_code ec, std::size_t bytes)
            {
                if (ec == http::error::end_of_stream)
                {
                    beast::error_code shutdown_ec;

                    self->socket_.shutdown(
                        tcp::socket::shutdown_both,
                        shutdown_ec);

                    return;
                }

                if (ec)
                {
                    std::cerr
                        << "Read error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                // Identify the client by IP address.
                beast::error_code endpoint_ec;

                auto endpoint =
                    self->socket_.remote_endpoint(endpoint_ec);

                if (endpoint_ec)
                {
                    std::cerr
                        << "Client endpoint error: "
                        << endpoint_ec.message()
                        << '\n';

                    return;
                }

                self->client_id_ =
                    endpoint.address().to_string();

                if (self->rate_limiting_enabled_)
                {
                    self->check_rate_limit();
                }
                else
                {
                    self->connect_to_backend();
                }
            });
    }

    // ============================================================
    // Rate Limiting
    // ============================================================

    void check_rate_limit()
    {
        auto self = shared_from_this();


        rate_limiter_->async_allow(
            client_id_,
            [self](bool allowed)
            {
                if (!allowed)
                {
                    self->send_rate_limit_response();
                    return;
                }

                self->connect_to_backend();
            });
    }

    void send_rate_limit_response()
    {
        auto self = shared_from_this();

        rate_limit_response_ =
            http::response<http::string_body>(
                http::status::too_many_requests,
                request_.version());

        rate_limit_response_.set(
            http::field::content_type,
            "text/plain");

        rate_limit_response_.body() =
            "Too Many Requests";

        rate_limit_response_.prepare_payload();

        http::async_write(
            socket_,
            rate_limit_response_,
            [self](
                beast::error_code ec,
                std::size_t bytes)
            {
                if (ec)
                {
                    std::cerr
                        << "Rate limit response error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                beast::error_code shutdown_ec;

                self->socket_.shutdown(
                    tcp::socket::shutdown_send,
                    shutdown_ec);
            });
    }

    // ============================================================
    // Load Balancer -> Backend
    // ============================================================

    void connect_to_backend()
    {
        if (backend_connected_)
        {
            send_to_backend();
            return;
        }

        auto self = shared_from_this();

        resolver_.async_resolve(
            selected_backend_->host,
            std::to_string(selected_backend_->port),

            [self](
                beast::error_code ec,
                tcp::resolver::results_type results)
            {
                if (ec)
                {
                    std::cerr
                        << "Backend resolve error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                asio::async_connect(
                    self->backend_socket_,
                    results,

                    [self](
                        beast::error_code ec,
                        const tcp::endpoint &)
                    {
                        if (ec)
                        {
                            std::cerr
                                << "Backend connection error: "
                                << ec.message()
                                << '\n';

                            return;
                        }

                        self->backend_connected_ = true;

                        self->send_to_backend();
                    });
            });
    }

    // ============================================================
    // Gateway -> Backend
    // ============================================================

    void send_to_backend()
    {
        auto self = shared_from_this();

        http::async_write(
            backend_socket_,
            request_,

            [self](
                beast::error_code ec,
                std::size_t bytes)
            {
                if (ec)
                {
                    std::cerr
                        << "Backend write error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                self->read_from_backend();
            });
    }

    // ============================================================
    // Backend -> Gateway
    // ============================================================

    void read_from_backend()
    {
        auto self = shared_from_this();

        http::async_read(
            backend_socket_,
            backend_buffer_,
            backend_response_,

            [self](
                beast::error_code ec,
                std::size_t bytes)
            {
                if (ec)
                {
                    std::cerr
                        << "Backend read error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                self->send_to_client();
            });
    }

    // ============================================================
    // Gateway -> Client
    // ============================================================

    void send_to_client()
    {
        auto self = shared_from_this();

        backend_response_.version(request_.version());

        http::async_write(
            socket_,
            backend_response_,

            [self](
                beast::error_code ec,
                std::size_t bytes)
            {
                if (ec)
                {
                    std::cerr
                        << "Client write error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                // Response has been sent successfully.
                // Wait for another request on the same client connection.
                self->request_ = {};
                self->backend_response_ = {};

                self->read_request();
            });
    }
};

class Server
{
private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;

    std::shared_ptr<LoadBalancer> load_balancer_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    bool rate_limiting_enabled_;

public:
    Server(
        asio::io_context &io_context,
        unsigned short port,
        std::shared_ptr<LoadBalancer> load_balancer,
        std::shared_ptr<RateLimiter> rate_limiter,
        bool rate_limiting_enabled)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), port)),
          load_balancer_(std::move(load_balancer)),
          rate_limiter_(std::move(rate_limiter)),
          rate_limiting_enabled_(rate_limiting_enabled)
    {
        accept();
    }

private:
    void accept()
    {
        acceptor_.async_accept(
            [this](
                beast::error_code ec,
                tcp::socket socket)
            {
                if (!ec)
                {
                    std::make_shared<Session>(
                        std::move(socket),
                        load_balancer_,
                        rate_limiter_,
                        rate_limiting_enabled_)
                        ->start();
                }
                else
                {
                    std::cerr
                        << "Accept error: "
                        << ec.message()
                        << '\n';
                }

                accept();
            });
    }
};

int main()
{
    try
    {
        asio::io_context io_context;
        constexpr bool rate_limiting_enabled = true;

        // ========================================================
        // Redis-backed Rate Limiter
        // ========================================================

        auto rate_limit_store =
            std::make_shared<RedisRateLimitStore>(
                io_context);

        auto rate_limiter =
            std::make_shared<RateLimiter>(
                *rate_limit_store,
                100000.0, // bucket capacity
                100000.0); // tokens refilled per second

        // ========================================================
        // Load Balancer
        // ========================================================

        auto load_balancer =
            std::make_shared<LoadBalancer>(
                std::vector<Backend>{
                    Backend("127.0.0.1", 9001),
                    Backend("127.0.0.1", 9002),
                    Backend("127.0.0.1", 9003)});

        // ========================================================
        // Health Checker
        // ========================================================

        auto health_checker =
            std::make_shared<HealthChecker>(
                io_context,
                load_balancer);

        health_checker->start();

        // ========================================================
        // HTTP Server
        // ========================================================

        Server server(
            io_context,
            8080,
            load_balancer,
            rate_limiter,
            rate_limiting_enabled
        );

        std::cout
            << "Async server listening on port 8080...\n";

        std::cout
            << "Rate limiting: "
            << (rate_limiting_enabled ? "enabled" : "disabled")
            << '\n';

        io_context.run();
    }
    catch (const std::exception &e)
    {
        std::cerr
            << "Fatal error: "
            << e.what()
            << '\n';

        return 1;
    }

    return 0;
}