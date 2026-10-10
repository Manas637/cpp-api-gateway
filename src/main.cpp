#include <csignal>
#include <functional>
#include <iostream>
#include <memory>

#include <boost/asio.hpp>

#include "gateway_config.hpp"
#include "health_checker.hpp"
#include "load_balancer.hpp"
#include "rate_limiter.hpp"
#include "redis_rate_limit_store.hpp"
#include "server.hpp"
#include "metrics.hpp"
#include "config.hpp"

namespace asio = boost::asio;

int main()
{
    try
    {
        asio::io_context io_context;

        auto metrics = std::make_shared<Metrics>();

        GatewayConfig config = load_config();

        auto rate_limit_store =
            std::make_shared<RedisRateLimitStore>(
                io_context,
                config.redis_host,
                config.redis_port);

        auto rate_limiter =
            std::make_shared<RateLimiter>(
                *rate_limit_store,
                config.rate_limit_capacity,
                config.rate_limit_refill_rate);

        auto load_balancer =
            std::make_shared<LoadBalancer>(
                config.backends);

        load_balancer->set_strategy(
            config.load_balancing_strategy);

        auto health_checker =
            std::make_shared<HealthChecker>(
                io_context,
                load_balancer);

        health_checker->start();

        Server server(
            io_context,
            config,
            load_balancer,
            rate_limiter,
            metrics,
            config.rate_limiting_enabled);

        std::cout
            << "Async server listening on port "
            << config.port
            << "...\n";

        std::cout
            << "Rate limiting: "
            << (config.rate_limiting_enabled ? "enabled" : "disabled")
            << '\n';

        asio::signal_set signals(io_context, SIGINT, SIGTERM);

        bool shutdown_requested = false;

        std::function<void(boost::system::error_code, int)> on_signal;

        on_signal = [&](boost::system::error_code ec, int)
        {
            if (ec)
            {
                return;
            }

            if (!shutdown_requested)
            {
                shutdown_requested = true;

                std::cout
                    << "\nShutdown signal received; draining connections "
                    << "(grace period "
                    << config.shutdown_grace_period.count()
                    << " ms)...\n";

                health_checker->stop();

                server.begin_shutdown();
            }
            else
            {
                std::cout
                    << "Second shutdown signal received; forcing "
                    << "immediate shutdown.\n";

                server.force_shutdown();
            }

            signals.async_wait(on_signal);
        };

        signals.async_wait(on_signal);

        io_context.run();

        // The io_context has stopped (drained or forced); tear down
        // the Redis connection now that no request is in flight.
        rate_limit_store->shutdown();
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