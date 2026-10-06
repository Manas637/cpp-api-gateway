#include "config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

namespace
{
    const char *get_env(const char *name)
    {
        return std::getenv(name);
    }

    std::string get_string(
        const char *name,
        const std::string &default_value)
    {
        const char *value = get_env(name);

        if (!value)
            return default_value;

        return value;
    }

    unsigned short get_port(
        const char *name,
        unsigned short default_value)
    {
        const char *value = get_env(name);

        if (!value)
            return default_value;

        try
        {
            unsigned long port = std::stoul(value);

            if (port == 0 || port > 65535)
                throw std::invalid_argument("port out of range");

            return static_cast<unsigned short>(port);
        }
        catch (const std::exception &)
        {
            throw std::runtime_error(
                std::string("Invalid value for ") + name);
        }
    }

    double get_double(
        const char *name,
        double default_value)
    {
        const char *value = get_env(name);

        if (!value)
            return default_value;

        try
        {
            double result = std::stod(value);

            if (result < 0)
                throw std::invalid_argument("negative value");

            return result;
        }
        catch (const std::exception &)
        {
            throw std::runtime_error(
                std::string("Invalid value for ") + name);
        }
    }

    bool get_bool(
        const char *name,
        bool default_value)
    {
        const char *value = get_env(name);

        if (!value)
            return default_value;

        std::string str(value);

        if (str == "true" || str == "1")
            return true;

        if (str == "false" || str == "0")
            return false;

        throw std::runtime_error(
            std::string("Invalid boolean value for ") + name);
    }

    std::vector<Backend> parse_backends(const std::string &value)
    {
        std::vector<Backend> backends;

        std::stringstream ss(value);
        std::string entry;

        while (std::getline(ss, entry, ','))
        {
            if (entry.empty())
                throw std::runtime_error(
                    "Invalid BACKENDS entry: empty backend");

            const auto colon = entry.rfind(':');

            if (colon == std::string::npos ||
                colon == 0 ||
                colon == entry.size() - 1)
            {
                throw std::runtime_error(
                    "Invalid BACKENDS entry: " + entry);
            }

            const std::string host =
                entry.substr(0, colon);

            const std::string port_string =
                entry.substr(colon + 1);

            unsigned long port;

            try
            {
                port = std::stoul(port_string);
            }
            catch (const std::exception &)
            {
                throw std::runtime_error(
                    "Invalid backend port: " + port_string);
            }

            if (port == 0 || port > 65535)
            {
                throw std::runtime_error(
                    "Backend port out of range: " +
                    port_string);
            }

            backends.emplace_back(
                host,
                static_cast<unsigned short>(port));
        }

        if (backends.empty())
        {
            throw std::runtime_error(
                "BACKENDS must contain at least one backend");
        }

        return backends;
    }

    std::vector<Backend> get_backends()
    {
        const char *value = get_env("BACKENDS");

        if (!value)
        {
            return {
                Backend("127.0.0.1", 9001),
                Backend("127.0.0.1", 9002),
                Backend("127.0.0.1", 9003)};
        }

        return parse_backends(value);
    }
}

GatewayConfig load_config()
{
    GatewayConfig config;

    config.port =
        get_port("GATEWAY_PORT", config.port);

    config.backends = get_backends();

    config.rate_limiting_enabled =
        get_bool(
            "RATE_LIMITING_ENABLED",
            config.rate_limiting_enabled);

    config.rate_limit_capacity =
        get_double(
            "RATE_LIMIT_CAPACITY",
            config.rate_limit_capacity);

    config.rate_limit_refill_rate =
        get_double(
            "RATE_LIMIT_REFILL_RATE",
            config.rate_limit_refill_rate);

    config.redis_host =
        get_string(
            "REDIS_HOST",
            config.redis_host);

    config.redis_port =
        get_string(
            "REDIS_PORT",
            config.redis_port);

    return config;
}