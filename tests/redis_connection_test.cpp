#include <chrono>
#include <iostream>
#include <string>

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/redis/config.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/redis/src.hpp>

#include "redis_test_util.hpp"

namespace
{
    constexpr auto READINESS_TIMEOUT =
        std::chrono::milliseconds(1500);

    constexpr auto COMMAND_DEADLINE =
        std::chrono::seconds(3);
}

int main()
{
    const std::string host = redis_test::host();
    const std::string port = redis_test::port();

    // Fail fast with an explicit skip when Redis is absent instead of
    // letting Boost.Redis reconnect forever.
    if (!redis_test::reachable(
            host,
            port,
            READINESS_TIMEOUT))
    {
        redis_test::report_unavailable(host, port);
        return redis_test::SKIP_CODE;
    }

    boost::asio::io_context io_context;

    boost::redis::connection connection(io_context);

    boost::redis::request request;
    request.push("PING");

    boost::redis::response<std::string> response;

    bool completed = false;
    bool succeeded = false;

    const auto command_start =
        std::chrono::steady_clock::now();

    // Bound the command so a stalled server cannot hang the test, and
    // always stop the background run operation once we are done.
    connection.async_exec(
        request,
        response,
        boost::asio::cancel_after(
            COMMAND_DEADLINE,
            [&](boost::system::error_code ec,
                std::size_t /*bytes_transferred*/)
            {
                completed = true;

                if (ec)
                {
                    const auto elapsed =
                        std::chrono::steady_clock::now() -
                        command_start;

                    if (elapsed >= COMMAND_DEADLINE)
                    {
                        std::cerr
                            << "Redis command timed out after "
                            << std::chrono::duration_cast<
                                   std::chrono::seconds>(
                                   COMMAND_DEADLINE)
                                   .count()
                            << "s: "
                            << ec.message()
                            << '\n';
                    }
                    else
                    {
                        std::cerr
                            << "Redis error: "
                            << ec.message()
                            << '\n';
                    }
                }
                else
                {
                    const std::string value =
                        std::get<0>(response).value();

                    std::cout
                        << "Redis response: "
                        << value
                        << '\n';

                    succeeded = (value == "PONG");
                }

                // Cancel async_run() so io_context.run() can return.
                boost::asio::post(
                    io_context,
                    [&connection]()
                    {
                        connection.cancel();
                    });
            }));

    boost::redis::config config;
    config.addr.host = host;
    config.addr.port = port;

    connection.async_run(
        config,
        [](boost::system::error_code ec)
        {
            if (ec &&
                ec != boost::asio::error::operation_aborted)
            {
                std::cerr
                    << "Redis connection error: "
                    << ec.message()
                    << '\n';
            }
        });

    io_context.run();

    if (!completed)
    {
        std::cerr
            << "Redis operation did not complete within the deadline.\n";

        return 1;
    }

    if (!succeeded)
    {
        return 1;
    }

    std::cout
        << "Redis connection test passed!\n";

    return 0;
}
