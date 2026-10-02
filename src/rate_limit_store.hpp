#pragma once

#include <functional>
#include <string>

class RateLimitStore
{
public:
    using ConsumeHandler = std::function<void(bool allowed)>;

    virtual ~RateLimitStore() = default;

    virtual void async_consume(
        const std::string &client_id,
        double capacity,
        double refill_rate,
        ConsumeHandler handler) = 0;
};