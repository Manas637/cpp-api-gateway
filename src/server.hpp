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
#include <atomic>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

#include "gateway_config.hpp"
#include "load_balancer.hpp"
#include "rate_limiter.hpp"
#include "metrics.hpp"
#include "status_handler.hpp"
#include "logger.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

inline std::string generate_request_id()
{
    static std::atomic<std::uint64_t> counter{0};

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();

    std::ostringstream oss;
    oss << std::hex << now << "-" << counter.fetch_add(1);

    return oss.str();
}

class Session : public std::enable_shared_from_this<Session>
{
private:
    tcp::socket socket_;
    beast::flat_buffer buffer_;

    http::request<http::string_body> request_;

    tcp::resolver resolver_;
    tcp::socket backend_socket_;
    asio::steady_timer backend_response_timer_;
    std::chrono::milliseconds backend_response_timeout_;
    bool backend_response_timeout_triggered_ = false;

    asio::steady_timer backend_connect_timer_;
    std::chrono::milliseconds backend_connect_timeout_;

    // Test-only seam: when true, the first backend connect attempt is
    // left pending (DNS resolution is never started) so the connect
    // timeout can be exercised deterministically without a network
    // dependency. Consumed exactly once; always false in production.
    bool hold_first_connect_ = false;

    // Test-only seam: when true, the first backend connect attempt is
    // failed locally as if its connect timer had expired, determinis-
    // tically forcing a retry without depending on a timer tick or a
    // network dependency. Consumed exactly once; always false in
    // production.
    bool immediate_first_connect_timeout_ = false;

    // Overall request deadline. One absolute steady_clock deadline per
    // request, armed after the client request is fully read and shared
    // by every backend retry. A fresh deadline is armed for each
    // keep-alive request. It is cancelled when any response write is
    // initiated. It intentionally does not cover the initial request
    // read (idle keep-alive) nor the transmission of a produced
    // response.
    asio::steady_timer request_timer_;
    std::chrono::milliseconds request_timeout_;
    std::chrono::steady_clock::time_point request_deadline_;
    bool request_timeout_triggered_ = false;

    bool backend_attempt_active_ = false;
    std::uint64_t backend_attempt_id_ = 0;

    beast::flat_buffer backend_buffer_;
    http::response<http::string_body> backend_response_;

    http::response<http::string_body> rate_limit_response_;
    http::response<http::string_body> error_response_;

    std::shared_ptr<LoadBalancer> load_balancer_;
    std::shared_ptr<RateLimiter> rate_limiter_;

    bool rate_limiting_enabled_;

    Backend *selected_backend_ = nullptr;
    bool backend_connected_ = false;
    std::size_t retry_count_ = 0;

    // True while this session holds exactly one in_flight_requests
    // count on selected_backend_. The ownership flag makes acquire and
    // release idempotent, preventing leaks, double-decrements, and
    // underflow.
    bool backend_in_flight_counted_ = false;

    std::string client_id_;
    std::string request_id_;

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
        bool rate_limiting_enabled,
        std::chrono::milliseconds backend_response_timeout,
        std::chrono::milliseconds backend_connect_timeout,
        std::chrono::milliseconds request_timeout,
        bool test_hold_first_connect = false,
        bool test_immediate_first_connect_timeout = false)
        : socket_(std::move(socket)),
          resolver_(socket_.get_executor()),
          backend_socket_(socket_.get_executor()),
          backend_response_timer_(socket_.get_executor()),
          backend_connect_timer_(socket_.get_executor()),
          request_timer_(socket_.get_executor()),
          load_balancer_(std::move(load_balancer)),
          rate_limiter_(std::move(rate_limiter)),
          metrics_(std::move(metrics)),
          rate_limiting_enabled_(rate_limiting_enabled),
          backend_response_timeout_(backend_response_timeout),
          backend_connect_timeout_(backend_connect_timeout),
          request_timeout_(request_timeout),
          hold_first_connect_(test_hold_first_connect),
          immediate_first_connect_timeout_(
              test_immediate_first_connect_timeout)
    {
    }

    ~Session()
    {
        // If the session is torn down while a backend attempt is still
        // in flight (e.g. the io_context is stopped during shutdown),
        // return the slot so the backend in-flight accounting never
        // leaks. This is a no-op when no slot was acquired.
        release_backend_slot();
        mark_connection_closed();
    }

    void start()
    {
        mark_connection_opened();
        read_request();
    }

private:
    void start_request_timer()
    {
        auto self = shared_from_this();

        // One absolute deadline per request. The wait is never re-armed
        // by retries, so every backend attempt shares the same budget.
        request_deadline_ =
            request_started_at_ +
            request_timeout_;

        request_timeout_triggered_ = false;

        request_timer_.expires_at(request_deadline_);

        request_timer_.async_wait(
            [self](beast::error_code ec)
            {
                // Cancelled on response initiation or session teardown,
                // and stale re-entries, are no-ops.
                if (ec)
                {
                    return;
                }

                if (self->request_timeout_triggered_)
                {
                    return;
                }

                self->handle_request_timeout();
            });
    }

    void handle_request_timeout()
    {
        // Own the failure exactly once before mutating anything.
        if (request_timeout_triggered_)
        {
            return;
        }

        request_timeout_triggered_ = true;

        // Invalidate the current backend attempt so a pending
        // resolve/connect/write/read callback completing later is
        // inert and cannot record another failure, retry, or respond.
        backend_attempt_active_ = false;
        ++backend_attempt_id_;

        backend_connect_timer_.cancel();
        backend_response_timer_.cancel();

        // Abort outstanding backend operations. Their callbacks
        // complete with operation_aborted and return via the attempt
        // guard.
        resolver_.cancel();

        beast::error_code close_ec;
        backend_socket_.close(close_ec);

        // The backend socket is closed whether or not a backend attempt
        // was in flight, so a cached keep-alive connection is never
        // reused after this point.
        backend_connected_ = false;

        Logger::log(
            LogLevel::WARN,
            "request_timeout",
            "overall request timed out",
            request_id_);

        // Only attribute a backend failure when a backend attempt is
        // actually being abandoned. If the deadline fires while the
        // rate limiter or client path is pending, no backend is
        // involved and none should be penalized.
        if (backend_in_flight_counted_ &&
            selected_backend_ != nullptr)
        {
            release_backend_slot();

            metrics_->record_backend_failure();

            auto transition =
                selected_backend_
                    ->circuit_breaker
                    .record_failure();

            if (transition ==
                CircuitBreaker::Transition::OPENED)
            {
                metrics_->record_circuit_open();
            }
        }

        metrics_->record_request_timeout();

        send_gateway_timeout();
    }

    std::uint64_t start_backend_connect_timer()
    {
        auto self = shared_from_this();

        const std::uint64_t attempt_id = ++backend_attempt_id_;

        backend_attempt_active_ = true;

        backend_connect_timer_.expires_after(
            backend_connect_timeout_);

        backend_connect_timer_.async_wait(
            [self, attempt_id](beast::error_code ec)
            {
                // Ignore cancelled timers and callbacks from older
                // attempts.
                if (ec == asio::error::operation_aborted)
                {
                    return;
                }

                if (ec)
                {
                    Logger::log(
                        LogLevel::ERR,
                        "backend_connect_timer_failed",
                        ec.message(),
                        self->request_id_,
                        self->selected_backend_name());

                    return;
                }

                // The connect phase may already have completed, or the
                // request may already have exceeded its overall
                // deadline (handled separately).
                if (!self->backend_attempt_active_ ||
                    self->backend_attempt_id_ != attempt_id ||
                    self->request_timeout_triggered_)
                {
                    return;
                }

                self->handle_backend_connect_timeout();
            });

        return attempt_id;
    }

    void handle_backend_connect_timeout()
    {
        // Invalidate the attempt before doing anything else so a
        // pending resolve/connect callback returning later is inert and
        // cannot mutate state, record metrics, or retry a second time.
        backend_attempt_active_ = false;
        ++backend_attempt_id_;

        backend_connect_timer_.cancel();

        // Abort any pending DNS resolution and TCP connection
        // establishment. Both callbacks will complete with
        // operation_aborted and return early via the attempt guard.
        resolver_.cancel();

        beast::error_code close_ec;
        backend_socket_.close(close_ec);

        Logger::log(
            LogLevel::WARN,
            "backend_connect_timeout",
            "backend connect timed out",
            request_id_,
            selected_backend_name());

        release_backend_slot();

        metrics_->record_backend_failure();

        auto transition =
            selected_backend_->circuit_breaker.record_failure();

        if (transition == CircuitBreaker::Transition::OPENED)
        {
            metrics_->record_circuit_open();
        }

        backend_connected_ = false;

        if (retry_backend())
        {
            return;
        }

        send_service_unavailable();
    }

    std::uint64_t start_backend_response_timer()
    {
        auto self = shared_from_this();

        const std::uint64_t attempt_id = ++backend_attempt_id_;

        backend_attempt_active_ = true;
        backend_response_timeout_triggered_ = false;

        backend_response_timer_.expires_after(
            backend_response_timeout_);

        backend_response_timer_.async_wait(
            [self, attempt_id](beast::error_code ec)
            {
                // Ignore cancelled timers and callbacks from older attempts.
                if (ec == asio::error::operation_aborted)
                {
                    return;
                }

                if (ec)
                {
                    Logger::log(
                        LogLevel::ERR,
                        "backend_response_timer_failed",
                        ec.message(),
                        self->request_id_,
                        self->selected_backend_name());

                    return;
                }

                // The response may already have completed, or the
                // request may already have exceeded its overall
                // deadline (handled separately).
                if (!self->backend_attempt_active_ ||
                    self->backend_attempt_id_ != attempt_id ||
                    self->request_timeout_triggered_)
                {
                    return;
                }

                self->backend_response_timeout_triggered_ = true;

                Logger::log(
                    LogLevel::WARN,
                    "backend_response_timeout",
                    "backend response timed out",
                    self->request_id_,
                    self->selected_backend_name());

                // Closing the socket completes the pending async_read.
                // The read callback owns failure handling and retry.
                beast::error_code close_ec;
                self->backend_socket_.close(close_ec);
            });

        return attempt_id;
    }

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

    std::string selected_backend_name() const
    {
        if (selected_backend_ == nullptr)
        {
            return "";
        }

        return selected_backend_->host + ":" +
               std::to_string(selected_backend_->port);
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

    // -------------------------------------------------------------------------
    // Backend in-flight request accounting
    // -------------------------------------------------------------------------

    // Idempotent: each session holds at most one count on the backend
    // it is currently using.
    void acquire_backend_slot()
    {
        if (backend_in_flight_counted_ || selected_backend_ == nullptr)
        {
            return;
        }

        backend_in_flight_counted_ = true;

        selected_backend_->in_flight_requests.fetch_add(
            1, std::memory_order_relaxed);
    }

    // Idempotent: a slot is only ever released if this session holds
    // one, so repeated or stale calls cannot underflow the counter.
    void release_backend_slot()
    {
        if (!backend_in_flight_counted_ || selected_backend_ == nullptr)
        {
            return;
        }

        backend_in_flight_counted_ = false;

        selected_backend_->in_flight_requests.fetch_sub(
            1, std::memory_order_relaxed);
    }

    void close_client_connection()
    {
        // No pending request deadline may outlive the connection.
        request_timer_.cancel();

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

        // A response is about to be initiated; the request deadline no
        // longer applies.
        request_timer_.cancel();

        error_response_ =
            http::response<http::string_body>(
                http::status::service_unavailable,
                request_.version());

        error_response_.set(
            http::field::content_type,
            "text/plain");

        error_response_.set(
            "X-Request-ID",
            request_id_);

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
                    Logger::log(
                        LogLevel::ERR,
                        "service_unavailable_response_failed",
                        ec.message(),
                        self->request_id_);

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

    void send_gateway_timeout()
    {
        auto self = shared_from_this();

        // A response is about to be initiated; the request deadline no
        // longer applies. (The handler normally runs from the expired
        // request timer, so this is defensive.)
        request_timer_.cancel();

        error_response_ =
            http::response<http::string_body>(
                http::status::gateway_timeout,
                request_.version());

        error_response_.set(
            http::field::content_type,
            "text/plain");

        error_response_.set(
            "X-Request-ID",
            request_id_);

        error_response_.body() =
            "Gateway Timeout";

        error_response_.prepare_payload();

        // Record the HTTP response before starting the asynchronous write.
        self->metrics_->record_response(504);

        http::async_write(
            socket_,
            error_response_,
            [self](beast::error_code ec, std::size_t)
            {
                if (ec)
                {
                    Logger::log(
                        LogLevel::ERR,
                        "gateway_timeout_response_failed",
                        ec.message(),
                        self->request_id_);

                    self->close_client_connection();
                    return;
                }

                // The timeout response was successfully written to the
                // client, so the request has completed.
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
                    Logger::log(
                        LogLevel::ERR,
                        "client_read_failed",
                        ec.message(),
                        self->request_id_);

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

                if (self->request_.method() == http::verb::get &&
                    self->request_.target() == "/api/status")
                {
                    self->send_status_response();
                    return;
                }

                // Establish the request correlation ID.
                //
                // Preserve a client-provided X-Request-ID when present.
                // Otherwise generate one for this request.
                auto request_id_it = self->request_.find("X-Request-ID");

                if (request_id_it != self->request_.end() &&
                    !request_id_it->value().empty())
                {
                    self->request_id_ = std::string(request_id_it->value());
                }
                else
                {
                    self->request_id_ = generate_request_id();

                    self->request_.set(
                        "X-Request-ID",
                        self->request_id_);
                }

                self->metrics_->record_request();

                // Start measuring this HTTP request.
                // This is intentionally after the /metrics bypass.
                self->request_started_at_ =
                    std::chrono::steady_clock::now();

                self->retry_count_ = 0;

                // Arm the overall request deadline. It covers rate
                // limiting, DNS resolution, backend connect/write/read,
                // and any retries, but not the initial request read.
                self->start_request_timer();

                Logger::log(
                    LogLevel::INFO,
                    "request_started",
                    "",
                    self->request_id_);

                beast::error_code endpoint_ec;

                auto endpoint =
                    self->socket_.remote_endpoint(endpoint_ec);

                if (endpoint_ec)
                {
                    Logger::log(
                        LogLevel::ERR,
                        "client_endpoint_failed",
                        endpoint_ec.message(),
                        self->request_id_);

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
                // The overall deadline may have expired while the rate
                // limiter callback was pending. It must not start
                // backend work or send a second response.
                if (self->request_timeout_triggered_)
                {
                    return;
                }

                if (!allowed)
                {
                    self->metrics_->record_rate_limit_rejected();

                    Logger::log(
                        LogLevel::WARN,
                        "rate_limit_rejected",
                        "request rejected by rate limiter",
                        self->request_id_);

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

        // A response is about to be initiated; the request deadline no
        // longer applies.
        request_timer_.cancel();

        rate_limit_response_ =
            http::response<http::string_body>(
                http::status::too_many_requests,
                request_.version());

        rate_limit_response_.set(
            http::field::content_type,
            "text/plain");

        rate_limit_response_.set(
            "X-Request-ID",
            request_id_);

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
                    Logger::log(
                        LogLevel::ERR,
                        "rate_limit_response_failed",
                        ec.message(),
                        self->request_id_);

                    self->close_client_connection();
                    return;
                }

                // The 429 response was successfully written,
                // so record the completed request latency.
                self->record_request_duration();

                self->close_client_connection();
            });
    }

    bool can_retry() const
    {
        const auto method = request_.method();

        Logger::log(
            LogLevel::INFO,
            "retry_check",
            "checking retry eligibility",
            request_id_,
            selected_backend_name());

        if (retry_count_ >= 1)
            return false;

        return method == http::verb::get ||
               method == http::verb::head ||
               method == http::verb::put ||
               method == http::verb::delete_ ||
               method == http::verb::options;
    }

    bool retry_backend()
    {
        if (!can_retry())
            return false;

        ++retry_count_;

        Logger::log(
            LogLevel::WARN,
            "backend_retry",
            "retrying request on another backend",
            request_id_,
            selected_backend_name());

        // Clear state belonging to the failed backend attempt.
        backend_response_timer_.cancel();
        backend_connect_timer_.cancel();

        // Invalidate callbacks associated with the previous attempt.
        backend_attempt_active_ = false;
        ++backend_attempt_id_;
        backend_response_timeout_triggered_ = false;

        backend_buffer_.consume(backend_buffer_.size());
        backend_response_ = {};

        // Start the next backend attempt after the current async
        // callback has completely returned.
        // Preserve the failed backend pointer so the retry excludes it.
        Backend *failed_backend = selected_backend_;

        asio::post(
            socket_.get_executor(),
            [self = shared_from_this(), failed_backend]()
            {
                self->connect_to_backend(failed_backend);
            });

        return true;
    }

    // -------------------------------------------------------------------------
    // Backend connection
    // -------------------------------------------------------------------------

    void connect_to_backend(const Backend *excluded_backend = nullptr)
    {
        // Never start backend work for a request whose overall deadline
        // has already expired.
        if (request_timeout_triggered_)
        {
            return;
        }

        if (backend_connected_)
        {
            send_to_backend();
            return;
        }

        // A different backend is about to be selected (fresh attempt or
        // retry). Drop any slot still held for the previous backend
        // before the pointer changes so the count is never attributed
        // to the wrong backend.
        release_backend_slot();

        try
        {
            Backend *previous_backend = selected_backend_;

            CircuitBreaker::Transition transition =
                CircuitBreaker::Transition::NONE;

            selected_backend_ =
                &load_balancer_->next(&transition, excluded_backend);

            // Acquire immediately so resolve/connect time counts as an
            // in-flight request. send_to_backend() re-acquires
            // idempotently for the keep-alive reuse path.
            acquire_backend_slot();

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

            Logger::log(
                LogLevel::INFO,
                "backend_selected",
                "",
                request_id_,
                selected_backend_->host + ":" +
                    std::to_string(selected_backend_->port));
        }
        catch (const std::runtime_error &e)
        {
            Logger::log(
                LogLevel::ERR,
                "no_backend_available",
                e.what(),
                request_id_);

            send_service_unavailable();
            return;
        }

        auto self = shared_from_this();

        // Bound DNS resolution and TCP connection establishment with
        // the connect timeout. Arm it before async_resolve so the whole
        // connect phase is covered, and capture the attempt id so
        // callbacks that complete after a timeout are ignored.
        const std::uint64_t attempt_id =
            start_backend_connect_timer();

        // Test-only seam: fail the first attempt locally and
        // deterministically force a retry, without depending on a timer
        // tick or a network dependency.
        if (immediate_first_connect_timeout_)
        {
            immediate_first_connect_timeout_ = false;

            handle_backend_connect_timeout();
            return;
        }

        // Test-only seam: consume the hold and leave the attempt
        // pending with only the connect timer armed, so tests can
        // deterministically exercise the timeout without a network.
        if (hold_first_connect_)
        {
            hold_first_connect_ = false;
            return;
        }

        resolver_.async_resolve(
            selected_backend_->host,
            std::to_string(selected_backend_->port),

            [self, attempt_id](
                beast::error_code ec,
                tcp::resolver::results_type results)
            {
                // Ignore a callback whose attempt is no longer current or whose
                // request already exceeded its overall deadline.
                if (attempt_id != self->backend_attempt_id_ ||
                    !self->backend_attempt_active_ ||
                    self->request_timeout_triggered_)
                {
                    return;
                }

                if (ec)
                {
                    self->backend_connect_timer_.cancel();

                    self->release_backend_slot();

                    Logger::log(
                        LogLevel::ERR,
                        "backend_resolve_failed",
                        ec.message(),
                        self->request_id_,
                        self->selected_backend_name());

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

                    if (self->retry_backend())
                    {
                        return;
                    }

                    self->send_service_unavailable();
                    return;
                }

                asio::async_connect(
                    self->backend_socket_,
                    results,

                    [self, attempt_id](
                        beast::error_code ec,
                        const tcp::endpoint &)
                    {
                        // Ignore a callback whose attempt is no longer
                        // current or whose request already exceeded
                        // its overall deadline.
                        if (attempt_id != self->backend_attempt_id_ ||
                            !self->backend_attempt_active_ ||
                            self->request_timeout_triggered_)
                        {
                            return;
                        }

                        if (ec)
                        {
                            self->backend_connect_timer_.cancel();

                            self->release_backend_slot();

                            Logger::log(
                                LogLevel::ERR,
                                "backend_connection_failed",
                                ec.message(),
                                self->request_id_,
                                self->selected_backend_name());

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

                            if (self->retry_backend())
                            {
                                return;
                            }

                            self->send_service_unavailable();
                            return;
                        }

                        self->backend_connect_timer_.cancel();

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

        // Covers the keep-alive reuse path, where connect_to_backend()
        // writes to the already-connected backend without reselection.
        // On the fresh-connection path this is a no-op because the slot
        // was already acquired at selection.
        acquire_backend_slot();

        self->metrics_->record_backend_request();

        http::async_write(
            backend_socket_,
            request_,

            [self](
                beast::error_code ec,
                std::size_t)
            {
                // The overall deadline may have expired while the write
                // was pending; the request-timeout path already owns
                // failure accounting and the response.
                if (self->request_timeout_triggered_)
                {
                    return;
                }

                if (ec)
                {
                    self->release_backend_slot();

                    Logger::log(
                        LogLevel::ERR,
                        "backend_write_failed",
                        ec.message(),
                        self->request_id_,
                        self->selected_backend_name());

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

                    if (self->retry_backend())
                    {
                        return;
                    }

                    self->send_service_unavailable();
                    return;
                }

                self->read_from_backend();
            });
    }

    void handle_backend_response_timeout()
    {
        // The overall deadline may have expired first; the request
        // timeout path already owns failure accounting and the
        // response.
        if (request_timeout_triggered_)
        {
            return;
        }

        release_backend_slot();

        metrics_->record_backend_failure();

        auto transition =
            selected_backend_->circuit_breaker.record_failure();

        if (transition == CircuitBreaker::Transition::OPENED)
        {
            metrics_->record_circuit_open();
        }

        backend_connected_ = false;

        if (retry_backend())
        {
            return;
        }

        send_service_unavailable();
    }

    // -------------------------------------------------------------------------
    // Backend response
    // -------------------------------------------------------------------------

    void read_from_backend()
    {
        auto self = shared_from_this();

        const std::uint64_t attempt_id =
            start_backend_response_timer();

        http::async_read(
            backend_socket_,
            backend_buffer_,
            backend_response_,
            [self, attempt_id](
                beast::error_code ec,
                std::size_t)
            {
                // Ignore a callback if its attempt is no longer current
                // or the request already exceeded its overall deadline.
                if (attempt_id != self->backend_attempt_id_ ||
                    !self->backend_attempt_active_ ||
                    self->request_timeout_triggered_)
                {
                    return;
                }

                // The timer expired and closed the backend socket.
                // Handle this attempt's failure exactly once.
                if (self->backend_response_timeout_triggered_)
                {
                    self->backend_attempt_active_ = false;
                    self->backend_response_timeout_triggered_ = false;
                    self->backend_response_timer_.cancel();

                    self->handle_backend_response_timeout();
                    return;
                }

                // The read completed before the timeout was processed.
                self->backend_attempt_active_ = false;
                self->backend_response_timer_.cancel();

                if (ec)
                {
                    self->release_backend_slot();

                    Logger::log(
                        LogLevel::ERR,
                        "backend_read_failed",
                        ec.message(),
                        self->request_id_,
                        self->selected_backend_name());

                    self->metrics_->record_backend_failure();

                    auto transition =
                        self->selected_backend_->circuit_breaker
                            .record_failure();

                    if (transition ==
                        CircuitBreaker::Transition::OPENED)
                    {
                        self->metrics_->record_circuit_open();
                    }

                    beast::error_code close_ec;
                    self->backend_socket_.close(close_ec);

                    self->backend_connected_ = false;

                    if (self->retry_backend())
                    {
                        return;
                    }

                    self->send_service_unavailable();
                    return;
                }

                self->metrics_->record_backend_success();

                auto transition =
                    self->selected_backend_->circuit_breaker
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

        // The response is about to be initiated; the request deadline
        // no longer applies. Transmission of a produced response is not
        // aborted mid-write.
        request_timer_.cancel();

        backend_response_.version(
            request_.version());

        // Record before async_write so the response metric is updated
        // by the time the client receives the response.
        self->metrics_->record_response(
            backend_response_.result_int());

        backend_response_.set(
            "X-Request-ID",
            request_id_);

        http::async_write(
            socket_,
            backend_response_,

            [self](
                beast::error_code ec,
                std::size_t)
            {
                // The backend response is complete, so the backend
                // attempt is finished whether or not the client write
                // succeeded.
                self->release_backend_slot();

                if (ec)
                {
                    Logger::log(
                        LogLevel::ERR,
                        "client_write_failed",
                        ec.message(),
                        self->request_id_);

                    self->close_client_connection();
                    return;
                }

                // The complete response has been successfully written
                // to the client. Record end-to-end request latency.
                self->record_request_duration();

                Logger::log(
                    LogLevel::INFO,
                    "request_completed",
                    "request completed successfully",
                    self->request_id_);

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
                    Logger::log(
                        LogLevel::ERR,
                        "metrics_response_failed",
                        ec.message());

                    self->close_client_connection();
                    return;
                }

                // /metrics uses Connection: close semantics.
                // It intentionally does not contribute to request metrics.
                self->close_client_connection();
            });
    }

    void send_status_response()
    {
        auto response =
            std::make_shared<
                http::response<http::string_body>>(
                http::status::ok,
                request_.version());

        response->set(
            http::field::content_type,
            "application/json");

        response->set(
            http::field::server,
            "cpp-api-gateway");

        response->keep_alive(false);

        response->body() =
            StatusHandler::build_json(
                *load_balancer_,
                *metrics_,
                rate_limiting_enabled_);

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
                    Logger::log(
                        LogLevel::ERR,
                        "status_response_failed",
                        ec.message());

                    self->close_client_connection();
                    return;
                }

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

    std::chrono::milliseconds backend_response_timeout_;
    std::chrono::milliseconds backend_connect_timeout_;
    std::chrono::milliseconds request_timeout_;

    // Test-only seams: exercised only when true is passed to the
    // constructor (always false in production via main.cpp).
    bool test_hold_first_connect_ = false;
    bool test_immediate_first_connect_timeout_ = false;

public:
    Server(
        asio::io_context &io_context,
        const GatewayConfig &config,
        std::shared_ptr<LoadBalancer> load_balancer,
        std::shared_ptr<RateLimiter> rate_limiter,
        std::shared_ptr<Metrics> metrics,
        bool rate_limiting_enabled,
        bool test_hold_first_connect = false,
        bool test_immediate_first_connect_timeout = false)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), config.port)),
          load_balancer_(std::move(load_balancer)),
          rate_limiter_(std::move(rate_limiter)),
          metrics_(std::move(metrics)),
          rate_limiting_enabled_(rate_limiting_enabled),
          backend_response_timeout_(config.backend_response_timeout),
          backend_connect_timeout_(config.backend_connect_timeout),
          request_timeout_(config.request_timeout),
          test_hold_first_connect_(test_hold_first_connect),
          test_immediate_first_connect_timeout_(
              test_immediate_first_connect_timeout)
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
                        rate_limiting_enabled_,
                        backend_response_timeout_,
                        backend_connect_timeout_,
                        request_timeout_,
                        test_hold_first_connect_,
                        test_immediate_first_connect_timeout_)
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

                Logger::log(
                    LogLevel::ERR,
                    "accept_failed",
                    ec.message());

                accept();
            });
    }
};