#ifndef GATEWAY_TESTS_REDIS_TEST_UTIL_HPP
#define GATEWAY_TESTS_REDIS_TEST_UTIL_HPP

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

namespace redis_test
{
    // Return code that CTest turns into a "Skipped" result (see the
    // SKIP_RETURN_CODE test property in CMakeLists.txt).
    inline constexpr int SKIP_CODE = 77;

    inline std::string host()
    {
        const char *value = std::getenv("REDIS_HOST");

        return (value != nullptr && *value != '\0')
                   ? std::string(value)
                   : std::string("127.0.0.1");
    }

    inline std::string port()
    {
        const char *value = std::getenv("REDIS_PORT");

        return (value != nullptr && *value != '\0')
                   ? std::string(value)
                   : std::string("6379");
    }

    // Bounded TCP reachability probe. This deliberately avoids Boost.Redis
    // so a missing server cannot start its unbounded reconnect loop. Both the
    // name resolution and the connect are run asynchronously under the same
    // deadline, so a slow or failing DNS lookup cannot block past `timeout`.
    // Any listener on the endpoint counts as "reachable"; the real test then
    // exercises actual Redis behavior.
    inline bool reachable(
        const std::string &host,
        const std::string &port,
        std::chrono::milliseconds timeout)
    {
        namespace asio = boost::asio;
        using asio::ip::tcp;

        asio::io_context io_context;

        tcp::resolver resolver(io_context);
        tcp::socket socket(io_context);
        asio::steady_timer timer(io_context);

        bool connected = false;

        timer.expires_after(timeout);
        timer.async_wait(
            [&](const boost::system::error_code &ec)
            {
                if (!ec)
                {
                    boost::system::error_code ignored;

                    resolver.cancel();
                    socket.close(ignored);
                }
            });

        resolver.async_resolve(
            host,
            port,
            [&](const boost::system::error_code &ec,
                const tcp::resolver::results_type &endpoints)
            {
                if (ec)
                {
                    timer.cancel();

                    return;
                }

                asio::async_connect(
                    socket,
                    endpoints,
                    [&](const boost::system::error_code &connect_ec,
                        const tcp::endpoint &)
                    {
                        connected = !connect_ec;

                        timer.cancel();
                    });
            });

        io_context.run();

        return connected;
    }

    inline void report_unavailable(
        const std::string &host,
        const std::string &port)
    {
        std::cout
            << "SKIPPED: Redis is not reachable at "
            << host << ":" << port
            << ". Start Redis, or set REDIS_HOST/REDIS_PORT "
               "to point at a running instance.\n";
    }
}

#endif
