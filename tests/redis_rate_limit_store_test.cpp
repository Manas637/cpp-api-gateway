#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

#include <boost/asio/io_context.hpp>

#include "rate_limiter.hpp"
#include "redis_rate_limit_store.hpp"

std::string unique_client_id(const std::string &name)
{
    static std::atomic<unsigned long long> counter{0};

    const auto now =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();

    return name + "-" +
           std::to_string(now) + "-" +
           std::to_string(++counter);
}

bool allow_request(
    boost::asio::io_context &io_context,
    RateLimiter &limiter,
    const std::string &client_id)
{
    bool callback_called = false;
    bool allowed = false;

    limiter.async_allow(
        client_id,
        [&](bool result)
        {
            allowed = result;
            callback_called = true;

            // We only want to run the io_context
            // until this Redis operation completes.
            io_context.stop();
        });

    io_context.run();

    assert(callback_called);

    // Prepare the io_context for the next request.
    io_context.restart();

    return allowed;
}

void test_initial_burst()
{
    boost::asio::io_context io_context;

    RedisRateLimitStore store(
        io_context,
        "127.0.0.1",
        "6379");

    RateLimiter limiter(
        store,
        3,
        0.0);

    const std::string client_id =
        unique_client_id("redis-burst-test");

    // Start with a clean Redis bucket.
    assert(allow_request(
        io_context,
        limiter,
        client_id));

    assert(allow_request(
        io_context,
        limiter,
        client_id));

    assert(allow_request(
        io_context,
        limiter,
        client_id));

    // Capacity is exhausted.
    assert(!allow_request(
        io_context,
        limiter,
        client_id));

    std::cout
        << "Redis initial burst test passed!\n";
}

void test_refill()
{
    boost::asio::io_context io_context;

    RedisRateLimitStore store(
        io_context,
        "127.0.0.1",
        "6379");

    RateLimiter limiter(
        store,
        3,
        2.0);

    const std::string client_id =
        unique_client_id("redis-refill-test");

    assert(allow_request(
        io_context,
        limiter,
        client_id));

    assert(allow_request(
        io_context,
        limiter,
        client_id));

    assert(allow_request(
        io_context,
        limiter,
        client_id));

    // Bucket is empty.
    assert(!allow_request(
        io_context,
        limiter,
        client_id));

    // 2 tokens/sec × 0.6 sec ≈ 1.2 tokens.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(600));

    assert(allow_request(
        io_context,
        limiter,
        client_id));

    std::cout
        << "Redis refill test passed!\n";
}

void test_different_clients()
{
    boost::asio::io_context io_context;

    RedisRateLimitStore store(
        io_context,
        "127.0.0.1",
        "6379");

    RateLimiter limiter(
        store,
        1,
        0.0);

    const std::string client_id_a =
        unique_client_id("redis-client-a");

    const std::string client_id_b =
        unique_client_id("redis-client-b");

    assert(allow_request(
        io_context,
        limiter,
        client_id_a));

    assert(!allow_request(
        io_context,
        limiter,
        client_id_a));

    // Different client has its own bucket.
    assert(allow_request(
        io_context,
        limiter,
        client_id_b));

    std::cout
        << "Redis different clients test passed!\n";
}

int main()
{
    test_initial_burst();
    test_refill();
    test_different_clients();

    std::cout
        << "All Redis rate limiter tests passed!\n";

    return 0;
}