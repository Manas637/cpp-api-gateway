#include "rate_limiter.hpp"
#include <utility>

RateLimiter::RateLimiter(
    RateLimitStore &store,
    double capacity,
    double refill_rate)
    : store_(store),
      capacity_(capacity),
      refill_rate_(refill_rate)
{
}

void RateLimiter::async_allow(
    const std::string &client_id,
    AllowHandler handler)
{
    store_.async_consume(
        client_id,
        capacity_,
        refill_rate_,
        std::move(handler));
}