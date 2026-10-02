#include <iostream>

#include <boost/redis/src.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>

int main()
{
    boost::asio::io_context io_context;

    boost::redis::connection connection(io_context);

    boost::redis::request request;
    request.push("PING");

    boost::redis::response<std::string> response;

    connection.async_exec(
        request,
        response,
        [&](boost::system::error_code ec,
            std::size_t /*bytes_transferred*/)
        {
            if (ec)
            {
                std::cerr
                    << "Redis error: "
                    << ec.message()
                    << '\n';

                return;
            }

            std::cout
                << "Redis response: "
                << std::get<0>(response).value()
                << '\n';

            io_context.stop();
        });

    connection.async_run(
        boost::redis::config{},
        {},
        [&](boost::system::error_code ec)
        {
            if (ec)
            {
                std::cerr
                    << "Redis connection error: "
                    << ec.message()
                    << '\n';
            }
        });

    io_context.run();

    return 0;
}