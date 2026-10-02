#include <cassert>
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

int main()
{

    test_round_robin();
    test_skip_unhealthy_backend();
    test_all_backends_unhealthy();

    std::cout << "All LoadBalancer tests passed!\n";

    return 0;
}