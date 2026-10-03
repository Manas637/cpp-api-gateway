#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>

#include <string>

#include "rate_limit_store.hpp"

class RedisRateLimitStore : public RateLimitStore
{
public:
    explicit RedisRateLimitStore(
        boost::asio::io_context &io_context);

    void async_consume(
        const std::string &client_id,
        double capacity,
        double refill_rate,
        ConsumeHandler handler) override;

    void shutdown();

private:
    boost::redis::connection connection_;
};