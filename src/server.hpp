#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "gateway_config.hpp"
#include "load_balancer.hpp"
#include "rate_limiter.hpp"
#include "metrics.hpp"

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

    std::shared_ptr<Metrics> metrics_;

    // Ensures every accepted client connection contributes exactly
    // one increment and one decrement to active_connections.
    bool connection_tracked_ = false;

    // Start time of the currently processed HTTP request.
    // This is reset for every request on a keep-alive connection.
    std::chrono::steady_clock::time_point request_started_at_;

public:
    Session(
        tcp::socket socket,
        std::shared_ptr<LoadBalancer> load_balancer,
        std::shared_ptr<RateLimiter> rate_limiter,
        std::shared_ptr<Metrics> metrics,
        bool rate_limiting_enabled)
        : socket_(std::move(socket)),
          resolver_(socket_.get_executor()),
          backend_socket_(socket_.get_executor()),
          load_balancer_(std::move(load_balancer)),
          rate_limiter_(std::move(rate_limiter)),
          metrics_(std::move(metrics)),
          rate_limiting_enabled_(rate_limiting_enabled)
    {
    }

    ~Session()
    {
        mark_connection_closed();
    }

    void start()
    {
        mark_connection_opened();
        read_request();
    }

private:
    // -------------------------------------------------------------------------
    // Request latency metrics
    // -------------------------------------------------------------------------

    void record_request_duration()
    {
        const auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                request_started_at_);

        metrics_->record_request_duration(
            static_cast<std::uint64_t>(duration.count()));
    }

    // -------------------------------------------------------------------------
    // Connection lifecycle metrics
    // -------------------------------------------------------------------------

    void mark_connection_opened()
    {
        if (connection_tracked_)
            return;

        connection_tracked_ = true;

        metrics_->connection_opened();
    }

    void mark_connection_closed()
    {
        if (!connection_tracked_)
            return;

        connection_tracked_ = false;

        metrics_->connection_closed();
    }

    void close_client_connection()
    {
        mark_connection_closed();

        beast::error_code ec;

        socket_.shutdown(
            tcp::socket::shutdown_both,
            ec);

        socket_.close(ec);
    }

    // -------------------------------------------------------------------------
    // 503 response
    // -------------------------------------------------------------------------

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

        // Record the HTTP response before starting the asynchronous write.
        self->metrics_->record_response(503);

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

                    self->close_client_connection();
                    return;
                }

                // The response was successfully written to the client,
                // so the request has completed.
                self->record_request_duration();

                self->request_ = {};
                self->read_request();
            });
    }

    // -------------------------------------------------------------------------
    // Client request
    // -------------------------------------------------------------------------

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
                    self->close_client_connection();
                    return;
                }

                if (ec)
                {
                    std::cerr
                        << "Read error: "
                        << ec.message()
                        << '\n';

                    self->close_client_connection();
                    return;
                }

                // Prometheus endpoint intentionally bypasses the normal
                // request/rate-limit/backend pipeline.
                if (self->request_.method() == http::verb::get &&
                    self->request_.target() == "/metrics")
                {
                    self->send_metrics_response();
                    return;
                }

                self->metrics_->record_request();

                // Start measuring this HTTP request.
                // This is intentionally after the /metrics bypass.
                self->request_started_at_ =
                    std::chrono::steady_clock::now();

                beast::error_code endpoint_ec;

                auto endpoint =
                    self->socket_.remote_endpoint(endpoint_ec);

                if (endpoint_ec)
                {
                    std::cerr
                        << "Client endpoint error: "
                        << endpoint_ec.message()
                        << '\n';

                    self->close_client_connection();
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

    // -------------------------------------------------------------------------
    // Rate limiting
    // -------------------------------------------------------------------------

    void check_rate_limit()
    {
        auto self = shared_from_this();

        rate_limiter_->async_allow(
            client_id_,
            [self](bool allowed)
            {
                if (!allowed)
                {
                    self->metrics_->record_rate_limit_rejected();

                    self->send_rate_limit_response();
                    return;
                }

                self->metrics_->record_rate_limit_allowed();

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

        // Record the response before async_write.
        self->metrics_->record_response(429);

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

                    self->close_client_connection();
                    return;
                }

                // The 429 response was successfully written,
                // so record the completed request latency.
                self->record_request_duration();

                self->close_client_connection();
            });
    }

    // -------------------------------------------------------------------------
    // Backend connection
    // -------------------------------------------------------------------------

    void connect_to_backend()
    {
        if (backend_connected_)
        {
            send_to_backend();
            return;
        }

        try
        {
            Backend *previous_backend = selected_backend_;

            CircuitBreaker::Transition transition =
                CircuitBreaker::Transition::NONE;

            selected_backend_ =
                &load_balancer_->next(&transition);

            if (transition ==
                CircuitBreaker::Transition::HALF_OPENED)
            {
                metrics_->record_circuit_half_open();
            }

            if (previous_backend != nullptr &&
                previous_backend != selected_backend_)
            {
                metrics_->record_failover();
            }
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

                    self->metrics_->record_backend_failure();

                    auto transition =
                        self->selected_backend_
                            ->circuit_breaker
                            .record_failure();

                    if (transition ==
                        CircuitBreaker::Transition::OPENED)
                    {
                        self->metrics_->record_circuit_open();
                    }

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

                            self->metrics_->record_backend_failure();

                            auto transition =
                                self->selected_backend_
                                    ->circuit_breaker
                                    .record_failure();

                            if (transition ==
                                CircuitBreaker::Transition::OPENED)
                            {
                                self->metrics_->record_circuit_open();
                            }

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

    // -------------------------------------------------------------------------
    // Backend request
    // -------------------------------------------------------------------------

    void send_to_backend()
    {
        auto self = shared_from_this();

        self->metrics_->record_backend_request();

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

                    self->metrics_->record_backend_failure();

                    auto transition =
                        self->selected_backend_
                            ->circuit_breaker
                            .record_failure();

                    if (transition ==
                        CircuitBreaker::Transition::OPENED)
                    {
                        self->metrics_->record_circuit_open();
                    }

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

    // -------------------------------------------------------------------------
    // Backend response
    // -------------------------------------------------------------------------

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

                    self->metrics_->record_backend_failure();

                    auto transition =
                        self->selected_backend_
                            ->circuit_breaker
                            .record_failure();

                    if (transition ==
                        CircuitBreaker::Transition::OPENED)
                    {
                        self->metrics_->record_circuit_open();
                    }

                    beast::error_code close_ec;

                    self->backend_socket_.close(
                        close_ec);

                    self->backend_connected_ = false;

                    self->connect_to_backend();

                    return;
                }

                self->metrics_->record_backend_success();

                auto transition =
                    self->selected_backend_
                        ->circuit_breaker
                        .record_success();

                if (transition ==
                    CircuitBreaker::Transition::RECOVERED)
                {
                    self->metrics_->record_circuit_recovery();
                }

                self->send_to_client();
            });
    }

    // -------------------------------------------------------------------------
    // Client response
    // -------------------------------------------------------------------------

    void send_to_client()
    {
        auto self = shared_from_this();

        backend_response_.version(
            request_.version());

        // Record before async_write so the response metric is updated
        // by the time the client receives the response.
        self->metrics_->record_response(
            backend_response_.result_int());

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

                    self->close_client_connection();
                    return;
                }

                // The complete response has been successfully written
                // to the client. Record end-to-end request latency.
                self->record_request_duration();

                self->request_ = {};
                self->backend_response_ = {};

                self->read_request();
            });
    }

    // -------------------------------------------------------------------------
    // Prometheus metrics endpoint
    // -------------------------------------------------------------------------

    void send_metrics_response()
    {
        auto response =
            std::make_shared<
                http::response<http::string_body>>(
                http::status::ok,
                request_.version());

        response->set(
            http::field::content_type,
            "text/plain; version=0.0.4; charset=utf-8");

        response->set(
            http::field::server,
            "cpp-api-gateway");

        response->keep_alive(false);

        response->body() =
            metrics_->to_prometheus();

        response->prepare_payload();

        auto self = shared_from_this();

        http::async_write(
            socket_,
            *response,

            [self, response](
                beast::error_code ec,
                std::size_t)
            {
                if (ec)
                {
                    std::cerr
                        << "Metrics response error: "
                        << ec.message()
                        << '\n';

                    self->close_client_connection();
                    return;
                }

                // /metrics uses Connection: close semantics.
                // It intentionally does not contribute to request metrics.
                self->close_client_connection();
            });
    }
};

// =============================================================================
// Server
// =============================================================================

class Server
{
private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;

    std::shared_ptr<LoadBalancer> load_balancer_;
    std::shared_ptr<RateLimiter> rate_limiter_;

    bool rate_limiting_enabled_;

    std::shared_ptr<Metrics> metrics_;

public:
    Server(
        asio::io_context &io_context,
        const GatewayConfig &config,
        std::shared_ptr<LoadBalancer> load_balancer,
        std::shared_ptr<RateLimiter> rate_limiter,
        std::shared_ptr<Metrics> metrics,
        bool rate_limiting_enabled)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), config.port)),
          load_balancer_(std::move(load_balancer)),
          rate_limiter_(std::move(rate_limiter)),
          metrics_(std::move(metrics)),
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
                        metrics_,
                        rate_limiting_enabled_)
                        ->start();

                    accept();

                    return;
                }

                // When the acceptor/io_context is being shut down,
                // operation_aborted is expected. Do not recursively
                // schedule another accept in that case.
                if (ec == asio::error::operation_aborted)
                {
                    return;
                }

                std::cerr
                    << "Accept error: "
                    << ec.message()
                    << '\n';

                accept();
            });
    }
};