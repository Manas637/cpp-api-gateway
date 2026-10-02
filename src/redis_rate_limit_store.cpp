#include <boost/redis/src.hpp>

#include "redis_rate_limit_store.hpp"
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace
{
    const char *RATE_LIMIT_SCRIPT = R"lua(
local key = KEYS[1]

local capacity = tonumber(ARGV[1])
local refill_rate = tonumber(ARGV[2])

-- Use Redis server time so multiple gateway instances
-- share the same clock.
local redis_time = redis.call("TIME")

local now_ms =
    tonumber(redis_time[1]) * 1000
    + math.floor(tonumber(redis_time[2]) / 1000)

local tokens = redis.call("HGET", key, "tokens")
local last_refill = redis.call("HGET", key, "last_refill")

-- First request from this client.
if not tokens then
    tokens = capacity
    last_refill = now_ms
else
    tokens = tonumber(tokens)
    last_refill = tonumber(last_refill)

    local elapsed_ms = now_ms - last_refill
    local elapsed_seconds = elapsed_ms / 1000

    local tokens_to_add =
        elapsed_seconds * refill_rate

    tokens = math.min(
        capacity,
        tokens + tokens_to_add
    )

    last_refill = now_ms
end

local allowed = 0

if tokens >= 1 then
    tokens = tokens - 1
    allowed = 1
end

redis.call(
    "HSET",
    key,
    "tokens", tokens,
    "last_refill", last_refill
)

-- Expire inactive buckets.
local ttl_seconds

if refill_rate > 0 then
    ttl_seconds = math.ceil(capacity / refill_rate)
else
    ttl_seconds = 3600
end

ttl_seconds = math.max(1, ttl_seconds)

redis.call(
    "EXPIRE",
    key,
    ttl_seconds
)

return allowed
)lua";
}

RedisRateLimitStore::RedisRateLimitStore(
    boost::asio::io_context &io_context)
    : connection_(io_context)
{
    boost::redis::config config;

    config.addr.host = "127.0.0.1";
    config.addr.port = "6379";

    connection_.async_run(
        config,
        [this](boost::system::error_code ec)
        {
            if (ec)
            {
                std::cerr
                    << "Redis connection error: "
                    << ec.message()
                    << '\n';
            }
        });
}

void RedisRateLimitStore::async_consume(
    const std::string &client_id,
    double capacity,
    double refill_rate,
    ConsumeHandler handler)
{
    auto request =
        std::make_shared<boost::redis::request>();

    auto response =
        std::make_shared<boost::redis::response<int>>();

    const std::string key =
        "rate_limit:" + client_id;

    request->push(
        "EVAL",
        RATE_LIMIT_SCRIPT,
        "1",
        key,
        std::to_string(capacity),
        std::to_string(refill_rate));

    connection_.async_exec(
        *request,
        *response,
        [request,
         response,
         handler = std::move(handler)](boost::system::error_code ec,
                                       std::size_t)
        {
            if (ec)
            {
                std::cerr
                    << "Redis rate limit error: "
                    << ec.message()
                    << '\n';

                handler(false);
                return;
            }

            const int result =
                std::get<0>(*response).value();

            std::cout
                << "Redis EVAL result: "
                << result
                << '\n';

            handler(result == 1);
        });
}