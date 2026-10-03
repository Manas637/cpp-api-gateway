#include <cassert>
#include <chrono>
#include <iostream>

#include "circuit_breaker.hpp"

using namespace std::chrono_literals;

struct TestClock
{
    CircuitBreaker::TimePoint now =
        CircuitBreaker::TimePoint{};

    void advance(std::chrono::milliseconds duration)
    {
        now += duration;
    }
};

void test_starts_closed()
{
    CircuitBreaker breaker(3, 1s);

    assert(
        breaker.state() ==
        CircuitBreaker::State::CLOSED);

    assert(breaker.allow_request());
}

void test_failures_below_threshold()
{
    CircuitBreaker breaker(3, 1s);
    TestClock clock;

    breaker.record_failure(clock.now);
    breaker.record_failure(clock.now);

    assert(
        breaker.state() ==
        CircuitBreaker::State::CLOSED);

    assert(breaker.failure_count() == 2);
    assert(breaker.allow_request(clock.now));
}

void test_threshold_opens_circuit()
{
    CircuitBreaker breaker(3, 1s);
    TestClock clock;

    breaker.record_failure(clock.now);
    breaker.record_failure(clock.now);
    breaker.record_failure(clock.now);

    assert(
        breaker.state() ==
        CircuitBreaker::State::OPEN);

    assert(!breaker.allow_request(clock.now));
}

void test_success_resets_failures()
{
    CircuitBreaker breaker(3, 1s);
    TestClock clock;

    breaker.record_failure(clock.now);
    breaker.record_failure(clock.now);

    breaker.record_success();

    assert(
        breaker.state() ==
        CircuitBreaker::State::CLOSED);

    assert(breaker.failure_count() == 0);
    assert(breaker.allow_request(clock.now));
}

void test_open_before_timeout_rejects()
{
    CircuitBreaker breaker(1, 5s);
    TestClock clock;

    breaker.record_failure(clock.now);

    assert(
        breaker.state() ==
        CircuitBreaker::State::OPEN);

    clock.advance(4s);

    assert(!breaker.allow_request(clock.now));

    assert(
        breaker.state() ==
        CircuitBreaker::State::OPEN);
}

void test_open_transitions_to_half_open()
{
    CircuitBreaker breaker(1, 5s);
    TestClock clock;

    breaker.record_failure(clock.now);

    clock.advance(5s);

    assert(breaker.allow_request(clock.now));

    assert(
        breaker.state() ==
        CircuitBreaker::State::HALF_OPEN);
}

void test_half_open_allows_only_one_request()
{
    CircuitBreaker breaker(1, 5s);
    TestClock clock;

    breaker.record_failure(clock.now);

    clock.advance(5s);

    assert(breaker.allow_request(clock.now));

    assert(
        breaker.state() ==
        CircuitBreaker::State::HALF_OPEN);

    // Second request must not be allowed.
    assert(!breaker.allow_request(clock.now));
}

void test_half_open_success_closes_circuit()
{
    CircuitBreaker breaker(1, 5s);
    TestClock clock;

    breaker.record_failure(clock.now);

    clock.advance(5s);

    assert(breaker.allow_request(clock.now));

    breaker.record_success();

    assert(
        breaker.state() ==
        CircuitBreaker::State::CLOSED);

    assert(breaker.failure_count() == 0);
    assert(breaker.allow_request(clock.now));
}

void test_half_open_failure_reopens_circuit()
{
    CircuitBreaker breaker(1, 5s);
    TestClock clock;

    breaker.record_failure(clock.now);

    clock.advance(5s);

    assert(breaker.allow_request(clock.now));

    assert(
        breaker.state() ==
        CircuitBreaker::State::HALF_OPEN);

    // Probe failed.
    clock.advance(1ms);
    breaker.record_failure(clock.now);

    assert(
        breaker.state() ==
        CircuitBreaker::State::OPEN);

    // We just entered OPEN.
    assert(!breaker.allow_request(clock.now));
}

void test_multiple_cycles()
{
    CircuitBreaker breaker(2, 5s);
    TestClock clock;

    // First cycle.
    breaker.record_failure(clock.now);
    breaker.record_failure(clock.now);

    assert(
        breaker.state() ==
        CircuitBreaker::State::OPEN);

    clock.advance(5s);

    assert(breaker.allow_request(clock.now));

    assert(
        breaker.state() ==
        CircuitBreaker::State::HALF_OPEN);

    breaker.record_success();

    assert(
        breaker.state() ==
        CircuitBreaker::State::CLOSED);

    assert(breaker.failure_count() == 0);

    // Second cycle.
    breaker.record_failure(clock.now);
    breaker.record_failure(clock.now);

    assert(
        breaker.state() ==
        CircuitBreaker::State::OPEN);

    assert(!breaker.allow_request(clock.now));
}

int main()
{
    test_starts_closed();
    test_failures_below_threshold();
    test_threshold_opens_circuit();
    test_success_resets_failures();
    test_open_before_timeout_rejects();
    test_open_transitions_to_half_open();
    test_half_open_allows_only_one_request();
    test_half_open_success_closes_circuit();
    test_half_open_failure_reopens_circuit();
    test_multiple_cycles();

    std::cout
        << "All circuit breaker tests passed\n";

    return 0;
}