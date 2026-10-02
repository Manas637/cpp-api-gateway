#include "in_memory_rate_limit_store.hpp"

#include <algorithm>

void InMemoryRateLimitStore::async_consume(
    const std::string &client_id,
    double capacity,
    double refill_rate,
    ConsumeHandler handler)
{
    bool allowed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();

        auto it = buckets_.find(client_id);

        // First request from this client.
        if (it == buckets_.end())
        {
            buckets_.emplace(
                client_id,
                Bucket{
                    capacity - 1.0,
                    now});

            allowed = true;
        }
        else
        {
            Bucket &bucket = it->second;

            // Calculate elapsed time.
            std::chrono::duration<double> elapsed =
                now - bucket.last_refill;

            // Refill tokens.
            double tokens_to_add =
                elapsed.count() * refill_rate;

            bucket.tokens = std::min(
                capacity,
                bucket.tokens + tokens_to_add);

            bucket.last_refill = now;

            // Consume a token if available.
            if (bucket.tokens >= 1.0)
            {
                bucket.tokens -= 1.0;
                allowed = true;
            }
        }
    }

    // Call the handler after releasing the mutex.
    handler(allowed);
}