#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

#include "load_balancer.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

class HealthChecker
    : public std::enable_shared_from_this<HealthChecker>
{

private:
    asio::io_context &io_context_;
    std::shared_ptr<LoadBalancer> load_balancer_;

    asio::steady_timer timer_;

    static constexpr int CHECK_INTERVAL_SECONDS = 5;
    static constexpr int TIMEOUT_SECONDS = 5;

    class CheckSession
        : public std::enable_shared_from_this<CheckSession>
    {

    private:
        asio::io_context &io_context_;

        tcp::resolver resolver_;
        tcp::socket socket_;

        beast::flat_buffer buffer_;

        http::request<http::empty_body> request_;
        http::response<http::string_body> response_;

        asio::steady_timer timeout_timer_;

        Backend *backend_;

        bool finished_ = false;

    public:
        CheckSession(
            asio::io_context &io_context,
            Backend &backend)
            : io_context_(io_context),
              resolver_(io_context),
              socket_(io_context),
              timeout_timer_(io_context),
              backend_(&backend) {}

        void start()
        {
            timeout_timer_.expires_after(
                std::chrono::seconds(TIMEOUT_SECONDS));

            auto self = shared_from_this();

            timeout_timer_.async_wait(
                [self](boost::system::error_code ec)
                {
                    if (!ec && !self->finished_)
                    {
                        self->finish(
                            false,
                            "timeout");
                    }
                });

            std::cout
                << "Starting health check for "
                << backend_->host
                << ":"
                << backend_->port
                << std::endl;

            resolver_.async_resolve(
                backend_->host,
                std::to_string(backend_->port),

                [self](
                    boost::system::error_code ec,
                    tcp::resolver::results_type results)
                {
                    if (self->finished_)
                        return;

                    if (ec)
                    {
                        self->finish(
                            false,
                            "resolve failed: " + ec.message());
                        return;
                    }

                    std::cout
                        << "Resolved "
                        << self->backend_->host
                        << ":"
                        << self->backend_->port
                        << ", connecting..."
                        << std::endl;

                    asio::async_connect(
                        self->socket_,
                        results,

                        [self](
                            boost::system::error_code ec,
                            const tcp::endpoint &)
                        {
                            if (self->finished_)
                                return;

                            if (ec)
                            {
                                self->finish(
                                    false,
                                    "connect failed: " + ec.message());
                                return;
                            }

                            self->send_request();
                        });
                });
        }

    private:
        void send_request()
        {

            request_.version(11);
            request_.method(http::verb::get);
            request_.target("/health");

            request_.set(
                http::field::host,
                backend_->host);

            request_.set(
                http::field::connection,
                "close");

            request_.prepare_payload();

            auto self = shared_from_this();

            http::async_write(
                socket_,
                request_,

                [self](
                    boost::system::error_code ec,
                    std::size_t)
                {
                    if (self->finished_)
                        return;

                    if (ec)
                    {
                        self->finish(
                            false,
                            "write failed: " + ec.message());
                        return;
                    }

                    self->read_response();
                });
        }

        void read_response()
        {

            auto self = shared_from_this();

            http::async_read(
                socket_,
                buffer_,
                response_,

                [self](
                    boost::system::error_code ec,
                    std::size_t)
                {
                    if (self->finished_)
                        return;

                    if (ec)
                    {
                        self->finish(
                            false,
                            "read failed: " + ec.message());
                        return;
                    }

                    if (self->response_.result() == http::status::ok)
                    {

                        self->finish(
                            true,
                            "HTTP 200");
                    }
                    else
                    {

                        self->finish(
                            false,
                            "HTTP " +
                                std::to_string(
                                    self->response_.result_int()));
                    }
                });
        }

        void finish(
            bool healthy,
            const std::string &reason)
        {

            if (finished_)
                return;

            finished_ = true;

            backend_->healthy = healthy;

            timeout_timer_.cancel();

            boost::system::error_code ignored;

            socket_.shutdown(
                tcp::socket::shutdown_both,
                ignored);

            socket_.close(ignored);

            std::cout
                << "Health check "
                << backend_->host
                << ":"
                << backend_->port
                << " -> "
                << (healthy ? "healthy" : "unhealthy")
                << " ("
                << reason
                << ")"
                << std::endl;
        }
    };

public:
    HealthChecker(
        asio::io_context &io_context,
        std::shared_ptr<LoadBalancer> load_balancer)
        : io_context_(io_context),
          load_balancer_(std::move(load_balancer)),
          timer_(io_context) {}

    void start()
    {
        check_all_backends();
    }

private:
    void check_all_backends()
    {

        for (Backend &backend : load_balancer_->backends())
        {

            std::make_shared<CheckSession>(
                io_context_,
                backend)
                ->start();
        }

        timer_.expires_after(
            std::chrono::seconds(CHECK_INTERVAL_SECONDS));

        auto self = shared_from_this();

        timer_.async_wait(
            [self](boost::system::error_code ec)
            {
                if (!ec)
                {
                    self->check_all_backends();
                }
            });
    }
};