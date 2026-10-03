#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "gateway_config.hpp"
#include "load_balancer.hpp"
#include "rate_limiter.hpp"

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

    http::response<http::string_body> rate_limit_response_;
    http::response<http::string_body> error_response_;

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
        read_request();
    }

private:
    void send_service_unavailable()
    {
        auto self = shared_from_this();

        error_response_ =
            http::response<http::string_body>(
                http::status::service_unavailable,
                request_.version());

        error_response_.set(
            http::field::content_type,
            "text/plain");

        error_response_.body() =
            "Service Unavailable";

        error_response_.prepare_payload();

        http::async_write(
            socket_,
            error_response_,
            [self](beast::error_code ec, std::size_t)
            {
                if (ec)
                {
                    std::cerr
                        << "503 response error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                self->request_ = {};
                self->read_request();
            });
    }

    void read_request()
    {
        auto self = shared_from_this();

        http::async_read(
            socket_,
            buffer_,
            request_,
            [self](beast::error_code ec, std::size_t)
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
                std::size_t)
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

    void connect_to_backend()
    {
        if (backend_connected_)
        {
            send_to_backend();
            return;
        }

        try
        {
            selected_backend_ =
                &load_balancer_->next();
        }
        catch (const std::runtime_error &e)
        {
            std::cerr
                << "No backend available: "
                << e.what()
                << '\n';

            send_service_unavailable();
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

                    self->selected_backend_
                        ->circuit_breaker
                        .record_failure();

                    self->backend_connected_ = false;
                    self->connect_to_backend();

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

                            self->selected_backend_
                                ->circuit_breaker
                                .record_failure();

                            beast::error_code close_ec;

                            self->backend_socket_.close(
                                close_ec);

                            self->backend_connected_ = false;

                            self->connect_to_backend();

                            return;
                        }

                        self->backend_connected_ = true;

                        self->send_to_backend();
                    });
            });
    }

    void send_to_backend()
    {
        auto self = shared_from_this();

        http::async_write(
            backend_socket_,
            request_,

            [self](
                beast::error_code ec,
                std::size_t)
            {
                if (ec)
                {
                    std::cerr
                        << "Backend write error: "
                        << ec.message()
                        << '\n';

                    self->selected_backend_
                        ->circuit_breaker
                        .record_failure();

                    beast::error_code close_ec;

                    self->backend_socket_.close(
                        close_ec);

                    self->backend_connected_ = false;

                    self->connect_to_backend();

                    return;
                }

                self->read_from_backend();
            });
    }

    void read_from_backend()
    {
        auto self = shared_from_this();

        http::async_read(
            backend_socket_,
            backend_buffer_,
            backend_response_,

            [self](
                beast::error_code ec,
                std::size_t)
            {
                if (ec)
                {
                    std::cerr
                        << "Backend read error: "
                        << ec.message()
                        << '\n';

                    self->selected_backend_
                        ->circuit_breaker
                        .record_failure();

                    beast::error_code close_ec;

                    self->backend_socket_.close(
                        close_ec);

                    self->backend_connected_ = false;

                    self->connect_to_backend();

                    return;
                }

                self->selected_backend_
                    ->circuit_breaker
                    .record_success();

                self->send_to_client();
            });
    }

    void send_to_client()
    {
        auto self = shared_from_this();

        backend_response_.version(
            request_.version());

        http::async_write(
            socket_,
            backend_response_,

            [self](
                beast::error_code ec,
                std::size_t)
            {
                if (ec)
                {
                    std::cerr
                        << "Client write error: "
                        << ec.message()
                        << '\n';

                    return;
                }

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
        const GatewayConfig &config,
        std::shared_ptr<LoadBalancer> load_balancer,
        std::shared_ptr<RateLimiter> rate_limiter,
        bool rate_limiting_enabled)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), config.port)),
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