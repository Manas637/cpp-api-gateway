#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "rate_limiter.hpp"
#include "redis_rate_limit_store.hpp"
#include "redis_test_util.hpp"

void test_concurrent_requests()
{
    boost::asio::io_context io_context;

    RedisRateLimitStore store(
        io_context,
        redis_test::host(),
        redis_test::port());

    RateLimiter limiter(
        store,
        100,
        0.0);

    constexpr int thread_count = 20;
    constexpr int requests_per_thread = 10;
    constexpr int total_requests =
        thread_count * requests_per_thread;

    const std::string client_id =
        "redis-concurrency-test-" +
        std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch()
                .count());

    std::atomic<int> allowed{0};
    std::atomic<int> completed{0};

    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (int i = 0; i < thread_count; ++i)
    {
        workers.emplace_back([&]()
                             {
            for (int j = 0;
                 j < requests_per_thread;
                 ++j)
            {
                boost::asio::post(
                    io_context,
                    [&]()
                    {
                        limiter.async_allow(
                            client_id,
                            [&](bool result)
                            {
                                if (result)
                                {
                                    ++allowed;
                                }

                                ++completed;
                            });
                    });
            } });
    }

    for (auto &worker : workers)
    {
        worker.join();
    }

    // Process all posted requests and Redis callbacks.
    while (completed.load() < total_requests)
    {
        io_context.run_one();
    }

    assert(completed == total_requests);
    assert(allowed == 100);

    std::cout
        << "Redis concurrent requests allowed: "
        << allowed
        << '\n';

    std::cout
        << "Redis concurrent requests rejected: "
        << total_requests - allowed.load()
        << '\n';
}

int main()
{
    const std::string host = redis_test::host();
    const std::string port = redis_test::port();

    if (!redis_test::reachable(
            host,
            port,
            std::chrono::milliseconds(1500)))
    {
        redis_test::report_unavailable(host, port);
        return redis_test::SKIP_CODE;
    }

    test_concurrent_requests();

    std::cout
        << "Redis concurrency test passed!\n";

    return 0;
}