#include <iostream>
#include <memory>

#include <boost/asio.hpp>

#include "gateway_config.hpp"
#include "health_checker.hpp"
#include "load_balancer.hpp"
#include "rate_limiter.hpp"
#include "redis_rate_limit_store.hpp"
#include "server.hpp"

namespace asio = boost::asio;

int main()
{
    try
    {
        asio::io_context io_context;

        constexpr bool rate_limiting_enabled = true;

        GatewayConfig config{
            .port = 8080,
            .backends = {
                Backend("127.0.0.1", 9001),
                Backend("127.0.0.1", 9002),
                Backend("127.0.0.1", 9003)},
            .rate_limit_capacity = 100000.0,
            .rate_limit_refill_rate = 100000.0};

        auto rate_limit_store =
            std::make_shared<RedisRateLimitStore>(
                io_context);

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
            rate_limiting_enabled);

        std::cout
            << "Async server listening on port "
            << config.port
            << "...\n";

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