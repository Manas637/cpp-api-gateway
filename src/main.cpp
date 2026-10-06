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