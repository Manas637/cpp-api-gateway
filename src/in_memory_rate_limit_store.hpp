#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "rate_limit_store.hpp"

class InMemoryRateLimitStore : public RateLimitStore
{
public:
    void async_consume(
        const std::string &client_id,
        double capacity,
        double refill_rate,
        ConsumeHandler handler) override;

private:
    struct Bucket
    {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;
    };

    std::unordered_map<std::string, Bucket> buckets_;
    std::mutex mutex_;
};