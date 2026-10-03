#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

#include "load_balancer.hpp"

int main()
{
    using namespace std::chrono_literals;

    Backend backend_a("127.0.0.1", 9001);
    Backend backend_b("127.0.0.1", 9002);

    LoadBalancer load_balancer({std::move(backend_a),
                                std::move(backend_b)});

    // ------------------------------------------------------------
    // 1. Both backends initially have CLOSED circuits.
    // ------------------------------------------------------------

    assert(
        load_balancer.backends()[0].circuit_breaker.state() == CircuitBreaker::State::CLOSED);

    assert(
        load_balancer.backends()[1].circuit_breaker.state() == CircuitBreaker::State::CLOSED);

    // ------------------------------------------------------------
    // 2. Load balancer can select backend A.
    // ------------------------------------------------------------

    Backend &first = load_balancer.next();

    assert(first.port == 9001);

    // ------------------------------------------------------------
    // 3. Open backend A's circuit.
    // ------------------------------------------------------------

    auto now = CircuitBreaker::Clock::now();

    for (int i = 0; i < 5; ++i)
    {
        load_balancer.backends()[0]
            .circuit_breaker
            .record_failure(now);
    }

    assert(
        load_balancer.backends()[0].circuit_breaker.state() == CircuitBreaker::State::OPEN);

    // ------------------------------------------------------------
    // 4. Load balancer must skip backend A.
    // ------------------------------------------------------------

    Backend &second = load_balancer.next();

    assert(second.port == 9002);

    // ------------------------------------------------------------
    // 5. Close backend A's circuit after a successful recovery.
    // ------------------------------------------------------------

    load_balancer.backends()[0]
        .circuit_breaker
        .record_success();

    assert(
        load_balancer.backends()[0].circuit_breaker.state() == CircuitBreaker::State::CLOSED);

    // ------------------------------------------------------------
    // 6. Backend A is eligible again.
    //
    // We don't assume the exact next() result because the
    // load balancer maintains round-robin state.
    // We simply make enough selections to observe both backends.
    // ------------------------------------------------------------

    bool saw_backend_a = false;
    bool saw_backend_b = false;

    for (int i = 0; i < 4; ++i)
    {
        Backend &backend = load_balancer.next();

        if (backend.port == 9001)
            saw_backend_a = true;

        if (backend.port == 9002)
            saw_backend_b = true;
    }

    assert(saw_backend_a);
    assert(saw_backend_b);

    std::cout
        << "LoadBalancer + CircuitBreaker integration tests passed\n";

    return 0;
}