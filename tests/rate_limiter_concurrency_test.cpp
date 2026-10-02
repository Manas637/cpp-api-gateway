#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "in_memory_rate_limit_store.hpp"
#include "rate_limiter.hpp"

bool allow_request(
    RateLimiter &limiter,
    const std::string &client_id)
{
    bool allowed = false;

    limiter.async_allow(
        client_id,
        [&](bool result)
        {
            allowed = result;
        });

    return allowed;
}

void test_concurrent_requests()
{
    InMemoryRateLimitStore store;

    // Exactly 100 requests should be allowed.
    // No refill occurs during the test.
    RateLimiter limiter(store, 100, 0.0);

    constexpr int thread_count = 20;
    constexpr int requests_per_thread = 10;

    std::atomic<int> allowed{0};

    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (int i = 0; i < thread_count; ++i)
    {
        threads.emplace_back([&]()
                             {
            for (int j = 0; j < requests_per_thread; ++j)
            {
                if (allow_request(limiter, "client-1"))
                {
                    ++allowed;
                }
            } });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    // 20 * 10 = 200 total requests,
    // but the bucket only contains 100 tokens.
    assert(allowed == 100);

    std::cout
        << "Concurrent requests allowed: "
        << allowed
        << '\n';
}

int main()
{
    test_concurrent_requests();

    std::cout << "All concurrency tests passed!\n";

    return 0;
}