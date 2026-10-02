#pragma once

#include <functional>
#include <string>

#include "rate_limit_store.hpp"

class RateLimiter
{
public:
    using AllowHandler = std::function<void(bool allowed)>;

    RateLimiter(
        RateLimitStore &store,
        double capacity,
        double refill_rate);

    void async_allow(
        const std::string &client_id,
        AllowHandler handler);

private:
    RateLimitStore &store_;

    double capacity_;
    double refill_rate_;
};