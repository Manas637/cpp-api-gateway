#pragma once

#include <chrono>
#include <cstddef>

class CircuitBreaker
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    enum class State
    {
        CLOSED,
        OPEN,
        HALF_OPEN
    };

    explicit CircuitBreaker(
        std::size_t failure_threshold,
        std::chrono::milliseconds open_duration)
        : failure_threshold_(failure_threshold),
          open_duration_(open_duration)
    {
    }

    bool allow_request(TimePoint now = Clock::now())
    {
        if (state_ == State::CLOSED)
        {
            return true;
        }

        if (state_ == State::OPEN)
        {
            if (now - opened_at_ < open_duration_)
            {
                return false;
            }

            state_ = State::HALF_OPEN;
            half_open_request_in_flight_ = true;

            return true;
        }

        // HALF_OPEN:
        // Exactly one probe request is allowed.
        return false;
    }

    void record_success()
    {
        state_ = State::CLOSED;
        failure_count_ = 0;
        half_open_request_in_flight_ = false;
    }

    void record_failure(TimePoint now = Clock::now())
    {
        if (state_ == State::HALF_OPEN)
        {
            state_ = State::OPEN;
            opened_at_ = now;
            half_open_request_in_flight_ = false;

            return;
        }

        if (state_ != State::CLOSED)
        {
            return;
        }

        ++failure_count_;

        if (failure_count_ >= failure_threshold_)
        {
            state_ = State::OPEN;
            opened_at_ = now;
        }
    }

    State state() const
    {
        return state_;
    }

    std::size_t failure_count() const
    {
        return failure_count_;
    }

private:
    State state_ = State::CLOSED;

    std::size_t failure_count_ = 0;
    std::size_t failure_threshold_;

    std::chrono::milliseconds open_duration_;

    TimePoint opened_at_{};

    bool half_open_request_in_flight_ = false;
};