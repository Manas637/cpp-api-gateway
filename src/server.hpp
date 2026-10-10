#pragma once

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
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

// ============================================================
// Graceful shutdown coordinator
// ============================================================
//
// Owns the shutdown flag, the bounded grace-period timer and the
// live-session count. It is io_context-affine: every member is
// touched only from handlers running on the owning io_context's
// thread, so no synchronization is required.
//
// Lifetime: held by Server (one strong reference) and by every live
// Session. The grace-period handler captures shared_from_this(), so
// the coordinator cannot be destroyed while a timer callback is
// pending. Server detaches its force-close callback on destruction,
// so no callback can ever invoke a destroyed Server.
class ShutdownCoordinator
    : public std::enable_shared_from_this<ShutdownCoordinator>
{
public:
    ShutdownCoordinator(
        asio::io_context &io_context,
        std::chrono::milliseconds grace_period)
        : io_context_(io_context),
          grace_timer_(io_context),
          grace_period_(grace_period)
    {
    }

    bool shutting_down() const
    {
        return in_progress_;
    }

    // Installed by Server. Invoked only from a timer handler on the
    // io thread, while Server is still alive.
    void set_force_close(std::function<void()> callback)
    {
        force_close_ = std::move(callback);
    }

    // Clears the Server callback so a coordinator that outlives
    // Server (because pending Session handlers still reference it)
    // can never invoke a destroyed Server.
    void detach()
    {
        force_close_ = nullptr;
    }

    void session_opened()
    {
        ++active_sessions_;
    }

    void session_closed()
    {
        if (active_sessions_ == 0)
        {
            return;
        }

        --active_sessions_;

        if (in_progress_ && active_sessions_ == 0)
        {
            finish();
        }
    }

    // Begins graceful shutdown. Idempotent: a second call is a no-op.
    void begin()
    {
        if (in_progress_)
        {
            return;
        }

        in_progress_ = true;

        if (active_sessions_ == 0)
        {
            finish();
            return;
        }

        grace_timer_.expires_after(grace_period_);

        auto self = shared_from_this();

        grace_timer_.async_wait(
            [self](beast::error_code ec)
            {
                if (ec)
                {
                    return;
                }

                self->on_grace_expired();
            });
    }

    // Forces shutdown immediately, bypassing the grace period.
    // Idempotent.
    void force()
    {
        in_progress_ = true;

        if (finished_)
        {
            return;
        }

        if (force_close_)
        {
            force_close_();
        }

        finish();
    }

private:
    void on_grace_expired()
    {
        if (!in_progress_)
        {
            return;
        }

        if (force_close_)
        {
            force_close_();
        }

        finish();
    }

    void finish()
    {
        if (finished_)
        {
            return;
        }

        finished_ = true;

        grace_timer_.cancel();

        if (!io_context_.stopped())
        {
            io_context_.stop();
        }
    }

    asio::io_context &io_context_;
    asio::steady_timer grace_timer_;
    std::chrono::milliseconds grace_period_;

    std::function<void()> force_close_;

    bool in_progress_ = false;
    bool finished_ = false;
    std::size_t active_sessions_ = 0;
};

class Session : public std::enable_shared_from_this<Session>
{
    // Server tracks live sessions and force-closes them once the grace
    // period expires; force_close() remains a private detail.
    friend class Server;

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

    std::shared_ptr<ShutdownCoordinator> shutdown_;

    // Ensures each session contributes exactly one increment and one
    // decrement to the shutdown coordinator's live-session count.
    bool shutdown_counted_ = false;

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
        std::shared_ptr<ShutdownCoordinator> shutdown,
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
          shutdown_(std::move(shutdown)),
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
        mark_session_closed();
    }

    void start()
    {
        mark_connection_opened();
        mark_session_opened();
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
    // Shutdown participation
    // -------------------------------------------------------------------------

    // Idempotent. Paired with mark_session_closed(); ensures the live
    // session count reaches zero exactly once per connection.
    void mark_session_opened()
    {
        if (shutdown_counted_)
            return;

        shutdown_counted_ = true;

        shutdown_->session_opened();
    }

    void mark_session_closed()
    {
        if (!shutdown_counted_)
            return;

        shutdown_counted_ = false;

        shutdown_->session_closed();
    }

    // Forced shutdown at grace-period expiry: cancel every timer,
    // abandon any in-flight backend attempt, release its in-flight
    // count and drop the client connection.
    void force_close()
    {
        request_timer_.cancel();
        backend_connect_timer_.cancel();
        backend_response_timer_.cancel();

        backend_attempt_active_ = false;

        beast::error_code ec;

        resolver_.cancel();

        backend_socket_.close(ec);

        backend_connected_ = false;

        release_backend_slot();

        close_client_connection();
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
        mark_session_closed();

        beast::error_code ec;

        socket_.shutdown(
            tcp::socket::shutdown_both,
            ec);

        socket_.close(ec);
    }

    // -------------------------------------------------------------------------
    // 503 response
    // -------------------------------------------------------------------------

    void send_service_unavailable(bool close_connection = false)
    {
        auto self = shared_from_this();

        // A response is about to be initiated; the request deadline no
        // longer applies.
        request_timer_.cancel();

        error_response_ =
            http::response<http::string_body>(
                http::status::service_unavailable,
                request_.version());

        // A shutdown rejection advertises Connection: close so the
        // client does not attempt to reuse the connection.
        error_response_.keep_alive(
            !(close_connection || shutdown_->shutting_down()));

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
            [self, close_connection](beast::error_code ec, std::size_t)
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

                if (close_connection ||
                    self->shutdown_->shutting_down())
                {
                    self->close_client_connection();
                    return;
                }

                // Back to idle: keep-alive wait for the next request.
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

        // During drain the connection is closed after this response;
        // advertise that so the client does not attempt to reuse it.
        if (shutdown_->shutting_down())
        {
            error_response_.keep_alive(false);
        }

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

                if (self->shutdown_->shutting_down())
                {
                    self->close_client_connection();
                    return;
                }

                // Back to idle: keep-alive wait for the next request.
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

                // While draining, refuse every new request with a clear
                // signal (503 + Connection: close) instead of starting
                // more work. Idle keep-alive connections stay open
                // until they are answered this way or force-closed at
                // the end of the grace period.
                if (self->shutdown_->shutting_down())
                {
                    self->request_id_ = generate_request_id();

                    self->request_.set(
                        "X-Request-ID",
                        self->request_id_);

                    self->metrics_->record_request();

                    self->request_started_at_ =
                        std::chrono::steady_clock::now();

                    self->send_service_unavailable(true);

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

        // The connection is always closed after this response;
        // advertise that consistently.
        rate_limit_response_.keep_alive(false);

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

        // During drain the connection is closed after this response;
        // advertise that so the client does not attempt to reuse it.
        if (shutdown_->shutting_down())
        {
            backend_response_.keep_alive(false);
        }

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

                // During drain, finish the in-flight request and close
                // instead of starting another keep-alive request.
                if (self->shutdown_->shutting_down())
                {
                    self->close_client_connection();
                    return;
                }

                // Back to idle: keep-alive wait for the next request.
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

    // Graceful-shutdown state. shutdown_ is shared with every Session;
    // sessions_ holds weak references so Server never extends a
    // Session's lifetime.
    std::shared_ptr<ShutdownCoordinator> shutdown_;
    std::vector<std::weak_ptr<Session>> sessions_;
    bool shutdown_started_ = false;

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
              test_immediate_first_connect_timeout),
          shutdown_(
              std::make_shared<ShutdownCoordinator>(
                  io_context,
                  config.shutdown_grace_period))
    {
        accept();
    }

    ~Server()
    {
        // Cancel the accept loop and make sure no pending shutdown
        // callback references this destroyed Server.
        if (shutdown_)
        {
            shutdown_->detach();
        }

        beast::error_code ec;
        acceptor_.close(ec);
    }

    // Begins graceful shutdown. Idempotent: safe to call repeatedly.
    // Must run on the io_context's thread.
    void begin_shutdown()
    {
        if (shutdown_started_)
        {
            return;
        }

        shutdown_started_ = true;

        // 1. Stop accepting new connections.
        beast::error_code ec;
        acceptor_.close(ec);

        // Install the forced-close hook and arm the grace period.
        // In-flight requests keep running to completion; sessions idle
        // on keep-alive stay open to be answered with a 503 if a new
        // request arrives, and are force-closed when the grace period
        // expires.
        shutdown_->set_force_close(
            [this]()
            {
                force_close_sessions();
            });

        shutdown_->begin();
    }

    // Forces shutdown immediately, bypassing the grace period.
    void force_shutdown()
    {
        if (!shutdown_started_)
        {
            begin_shutdown();
        }

        shutdown_->force();
    }

private:
    void force_close_sessions()
    {
        for (auto &weak : sessions_)
        {
            if (auto session = weak.lock())
            {
                session->force_close();
            }
        }
    }

    void accept()
    {
        acceptor_.async_accept(
            [this](
                beast::error_code ec,
                tcp::socket socket)
            {
                if (!ec)
                {
                    // A connection may be accepted in the race window
                    // just before shutdown closes the acceptor.
                    if (shutdown_->shutting_down())
                    {
                        beast::error_code close_ec;
                        socket.close(close_ec);
                        return;
                    }

                    // Drop weak references to sessions that have ended.
                    std::erase_if(
                        sessions_,
                        [](const std::weak_ptr<Session> &weak)
                        {
                            return weak.expired();
                        });

                    auto session = std::make_shared<Session>(
                        std::move(socket),
                        load_balancer_,
                        rate_limiter_,
                        metrics_,
                        shutdown_,
                        rate_limiting_enabled_,
                        backend_response_timeout_,
                        backend_connect_timeout_,
                        request_timeout_,
                        test_hold_first_connect_,
                        test_immediate_first_connect_timeout_);

                    sessions_.push_back(session);

                    session->start();

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