#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "in_memory_rate_limit_store.hpp"
#include "rate_limiter.hpp"

// Helper for the current in-memory implementation.
//
// The store currently invokes the callback immediately.
// Later, the Redis implementation will invoke it asynchronously.
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

void test_initial_burst()
{
    InMemoryRateLimitStore store;
    RateLimiter limiter(store, 3, 1.0);

    assert(allow_request(limiter, "client-1"));
    assert(allow_request(limiter, "client-1"));
    assert(allow_request(limiter, "client-1"));

    // Bucket is now empty.
    assert(!allow_request(limiter, "client-1"));
}

void test_refill()
{
    InMemoryRateLimitStore store;
    RateLimiter limiter(store, 3, 2.0);

    // Consume all 3 tokens.
    assert(allow_request(limiter, "client-1"));
    assert(allow_request(limiter, "client-1"));
    assert(allow_request(limiter, "client-1"));

    // No tokens remain.
    assert(!allow_request(limiter, "client-1"));

    // Refill rate = 2 tokens/sec.
    // After 600ms, approximately 1.2 tokens
    // should have been added.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(600));

    assert(allow_request(limiter, "client-1"));
}

void test_different_clients()
{
    InMemoryRateLimitStore store;
    RateLimiter limiter(store, 1, 1.0);

    assert(allow_request(limiter, "client-1"));
    assert(!allow_request(limiter, "client-1"));

    // Different client has a separate bucket.
    assert(allow_request(limiter, "client-2"));
}

void test_capacity_limit()
{
    InMemoryRateLimitStore store;
    RateLimiter limiter(store, 3, 1.0);

    // Consume the entire bucket.
    assert(allow_request(limiter, "client-1"));
    assert(allow_request(limiter, "client-1"));
    assert(allow_request(limiter, "client-1"));

    // No token should remain.
    assert(!allow_request(limiter, "client-1"));

    // Wait long enough for one token to refill.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(1100));

    // One token should now be available.
    assert(allow_request(limiter, "client-1"));

    // Immediately consuming again should fail.
    assert(!allow_request(limiter, "client-1"));
}

int main()
{
    test_initial_burst();
    test_refill();
    test_different_clients();
    test_capacity_limit();

    std::cout << "All RateLimiter tests passed!\n";

    return 0;
}