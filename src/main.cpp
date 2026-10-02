#include <iostream>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

using tcp = asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session>
{
private:
    tcp::socket socket_;
    beast::flat_buffer buffer_;

    http::request<http::string_body> request_;

    tcp::resolver resolver_;
    tcp::socket backend_socket_;

    beast::flat_buffer backend_buffer_;
    http::response<http::string_body> backend_response_;

public:
    explicit Session(tcp::socket socket)
        : socket_(std::move(socket)),
          resolver_(socket_.get_executor()),
          backend_socket_(socket_.get_executor()) {}

    void start()
    {
        read_request();
    }

private:
    void read_request()
    {
        auto self = shared_from_this();

        http::async_read(
            socket_,
            buffer_,
            request_,
            [self](beast::error_code ec, std::size_t bytes)
            {
                if (ec)
                {
                    std::cerr
                        << "Read error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                std::cout
                    << "Received: "
                    << self->request_.method_string()
                    << " "
                    << self->request_.target()
                    << '\n';

                self->connect_to_backend();
            });
    }

    void connect_to_backend()
    {

        auto self = shared_from_this();

        resolver_.async_resolve(
            "localhost",
            "9001",

            [self](
                beast::error_code ec,
                tcp::resolver::results_type results)
            {
                if (ec)
                {

                    std::cerr
                        << "Backend resolve error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                asio::async_connect(
                    self->backend_socket_,
                    results,

                    [self](
                        beast::error_code ec,
                        const tcp::endpoint &)
                    {
                        if (ec)
                        {

                            std::cerr
                                << "Backend connection error: "
                                << ec.message()
                                << '\n';

                            return;
                        }

                        std::cout
                            << "Connected to backend\n";

                        self->send_to_backend();
                    });
            });
    }

    void send_to_backend()
    {

        auto self = shared_from_this();

        http::async_write(
            backend_socket_,
            request_,

            [self](
                beast::error_code ec,
                std::size_t bytes)
            {
                if (ec)
                {

                    std::cerr
                        << "Backend write error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                self->read_from_backend();
            });
    }

    void read_from_backend()
    {

        auto self = shared_from_this();

        http::async_read(
            backend_socket_,
            backend_buffer_,
            backend_response_,

            [self](
                beast::error_code ec,
                std::size_t bytes)
            {
                if (ec)
                {

                    std::cerr
                        << "Backend read error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                self->send_to_client();
            });
    }

    void send_to_client()
    {

        auto self = shared_from_this();

        http::async_write(
            socket_,
            backend_response_,

            [self](
                beast::error_code ec,
                std::size_t bytes)
            {
                if (ec)
                {

                    std::cerr
                        << "Client write error: "
                        << ec.message()
                        << '\n';

                    return;
                }

                beast::error_code shutdown_ec;

                self->socket_.shutdown(
                    tcp::socket::shutdown_send,
                    shutdown_ec);
            });
    }
};

class Server
{
private:
    asio::io_context &io_context_;
    tcp::acceptor acceptor_;

public:
    Server(
        asio::io_context &io_context,
        unsigned short port)
        : io_context_(io_context),
          acceptor_(
              io_context,
              tcp::endpoint(tcp::v4(), port))
    {

        accept();
    }

private:
    void accept()
    {

        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {

                    std::cout
                        << "Client connected\n";

                    std::make_shared<Session>(
                        std::move(socket))
                        ->start();
                }
                else
                {

                    std::cerr
                        << "Accept error: "
                        << ec.message()
                        << '\n';
                }

                accept();
            });
    }
};

int main()
{

    try
    {

        asio::io_context io_context;

        Server server(io_context, 8080);

        std::cout
            << "Async server listening on port 8080...\n";

        io_context.run();
    }
    catch (const std::exception &e)
    {

        std::cerr
            << "Fatal error: "
            << e.what()
            << '\n';

        return 1;
    }

    return 0;
}