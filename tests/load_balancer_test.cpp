#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

#include "load_balancer.hpp"

void test_round_robin()
{

    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002),
                     Backend("127.0.0.1", 9003)});

    assert(lb.next().port == 9001);
    assert(lb.next().port == 9002);
    assert(lb.next().port == 9003);
    assert(lb.next().port == 9001);
}

void test_skip_unhealthy_backend()
{

    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002),
                     Backend("127.0.0.1", 9003)});

    lb.backends()[1].healthy = false;

    assert(lb.next().port == 9001);
    assert(lb.next().port == 9003);
    assert(lb.next().port == 9001);
    assert(lb.next().port == 9003);
}

void test_all_backends_unhealthy()
{

    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002),
                     Backend("127.0.0.1", 9003)});

    for (auto &backend : lb.backends())
    {
        backend.healthy = false;
    }

    bool threw = false;

    try
    {
        lb.next();
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }

    assert(threw);
}

void test_least_connections_selects_lowest()
{
    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002),
                     Backend("127.0.0.1", 9003)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    lb.backends()[0].in_flight_requests.store(5);
    lb.backends()[1].in_flight_requests.store(1);
    lb.backends()[2].in_flight_requests.store(3);

    assert(lb.next().port == 9002);

    lb.backends()[1].in_flight_requests.store(10);

    assert(lb.next().port == 9003);
}

void test_least_connections_tie_break_rotates()
{
    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002),
                     Backend("127.0.0.1", 9003)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    // All counts are equal, so selection must rotate instead of
    // always returning the first backend.
    assert(lb.next().port == 9001);
    assert(lb.next().port == 9002);
    assert(lb.next().port == 9003);
    assert(lb.next().port == 9001);
}

void test_least_connections_skips_unhealthy()
{
    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002),
                     Backend("127.0.0.1", 9003)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    lb.backends()[0].healthy = false;
    lb.backends()[0].in_flight_requests.store(0);
    lb.backends()[1].in_flight_requests.store(4);
    lb.backends()[2].in_flight_requests.store(2);

    assert(lb.next().port == 9003);
}

void test_least_connections_skips_open_circuit()
{
    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    auto now = CircuitBreaker::Clock::now();

    for (int i = 0; i < 5; ++i)
    {
        lb.backends()[0].circuit_breaker.record_failure(now);
    }

    lb.backends()[0].in_flight_requests.store(0);
    lb.backends()[1].in_flight_requests.store(7);

    assert(lb.next().port == 9002);

    // The skipped OPEN circuit must not be probed or moved to HALF_OPEN.
    assert(lb.backends()[0].circuit_breaker.state() ==
           CircuitBreaker::State::OPEN);
}

void test_least_connections_recovers_open_backend()
{
    using namespace std::chrono_literals;

    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    auto past = CircuitBreaker::Clock::now() - 10s;

    for (int i = 0; i < 5; ++i)
    {
        lb.backends()[0].circuit_breaker.record_failure(past);
    }

    assert(lb.backends()[0].circuit_breaker.state() ==
           CircuitBreaker::State::OPEN);

    // Backend 0's cooldown has elapsed, so it must remain a recovery
    // candidate even though backend 1 is a healthy CLOSED backend.
    CircuitBreaker::Transition transition =
        CircuitBreaker::Transition::NONE;

    Backend &selected = lb.next(&transition);

    assert(selected.port == 9001);
    assert(transition == CircuitBreaker::Transition::HALF_OPENED);
    assert(lb.backends()[0].circuit_breaker.state() ==
           CircuitBreaker::State::HALF_OPEN);
    assert(lb.backends()[1].circuit_breaker.state() ==
           CircuitBreaker::State::CLOSED);
}

void test_least_connections_reports_half_open_transition()
{
    using namespace std::chrono_literals;

    LoadBalancer lb({Backend("127.0.0.1", 9001)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    auto past = CircuitBreaker::Clock::now() - 10s;

    for (int i = 0; i < 5; ++i)
    {
        lb.backends()[0].circuit_breaker.record_failure(past);
    }

    assert(lb.backends()[0].circuit_breaker.state() ==
           CircuitBreaker::State::OPEN);

    CircuitBreaker::Transition transition =
        CircuitBreaker::Transition::NONE;

    Backend &selected = lb.next(&transition);

    assert(selected.port == 9001);

    assert(transition == CircuitBreaker::Transition::HALF_OPENED);

    assert(lb.backends()[0].circuit_breaker.state() ==
           CircuitBreaker::State::HALF_OPEN);
}

void test_least_connections_excluded_never_selected()
{
    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    lb.backends()[0].in_flight_requests.store(0);
    lb.backends()[1].in_flight_requests.store(9);

    const Backend *excluded = &lb.backends()[0];

    assert(lb.next(nullptr, excluded).port == 9002);
}

void test_least_connections_no_eligible_throws()
{
    LoadBalancer lb({Backend("127.0.0.1", 9001),
                     Backend("127.0.0.1", 9002)});

    lb.set_strategy(LoadBalancingStrategy::LEAST_CONNECTIONS);

    for (auto &backend : lb.backends())
    {
        backend.healthy = false;
    }

    bool threw = false;

    try
    {
        lb.next();
    }
    catch (const std::runtime_error &)
    {
        threw = true;
    }

    assert(threw);
}

void test_strategies_differ_under_uneven_load()
{
    // ROUND_ROBIN ignores in-flight counts, so a fresh rotation
    // (starting at backend 0) picks A even though A is busy.
    LoadBalancer round_robin_lb({Backend("127.0.0.1", 9001),
                                 Backend("127.0.0.1", 9002)});

    round_robin_lb.backends()[0].in_flight_requests.store(5);
    round_robin_lb.backends()[1].in_flight_requests.store(0);

    assert(round_robin_lb.next().port == 9001);

    // LEAST_CONNECTIONS counts in-flight requests and must instead
    // pick the idle backend B.
    LoadBalancer least_connections_lb({Backend("127.0.0.1", 9001),
                                       Backend("127.0.0.1", 9002)});

    least_connections_lb.set_strategy(
        LoadBalancingStrategy::LEAST_CONNECTIONS);

    least_connections_lb.backends()[0].in_flight_requests.store(5);
    least_connections_lb.backends()[1].in_flight_requests.store(0);

    assert(least_connections_lb.next().port == 9002);

    // Restore the seeded counters.
    round_robin_lb.backends()[0].in_flight_requests.store(0);
    round_robin_lb.backends()[1].in_flight_requests.store(0);

    least_connections_lb.backends()[0].in_flight_requests.store(0);
    least_connections_lb.backends()[1].in_flight_requests.store(0);
}

int main()
{

    test_round_robin();
    test_skip_unhealthy_backend();
    test_all_backends_unhealthy();

    test_least_connections_selects_lowest();
    test_least_connections_tie_break_rotates();
    test_least_connections_skips_unhealthy();
    test_least_connections_skips_open_circuit();
    test_least_connections_recovers_open_backend();
    test_least_connections_reports_half_open_transition();
    test_least_connections_excluded_never_selected();
    test_least_connections_no_eligible_throws();
    test_strategies_differ_under_uneven_load();

    std::cout << "All LoadBalancer tests passed!\n";

    return 0;
}