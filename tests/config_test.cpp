#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <chrono>

#include "config.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    void set_env(const std::string &name, const std::string &value)
    {
#ifdef _WIN32
        if (_putenv_s(name.c_str(), value.c_str()) != 0)
            throw std::runtime_error("Failed to set environment variable");
#else
        if (setenv(name.c_str(), value.c_str(), 1) != 0)
            throw std::runtime_error("Failed to set environment variable");
#endif
    }

    void unset_env(const std::string &name)
    {
#ifdef _WIN32
        if (_putenv_s(name.c_str(), "") != 0)
            throw std::runtime_error("Failed to unset environment variable");
#else
        if (unsetenv(name.c_str()) != 0)
            throw std::runtime_error("Failed to unset environment variable");
#endif
    }

    void clear_config_environment()
    {
        unset_env("GATEWAY_PORT");
        unset_env("BACKENDS");
        unset_env("RATE_LIMITING_ENABLED");
        unset_env("RATE_LIMIT_CAPACITY");
        unset_env("RATE_LIMIT_REFILL_RATE");
        unset_env("REDIS_HOST");
        unset_env("REDIS_PORT");
        unset_env("BACKEND_CONNECT_TIMEOUT_MS");
        unset_env("BACKEND_RESPONSE_TIMEOUT_MS");
        unset_env("REQUEST_TIMEOUT_MS");
    }

    void expect(bool condition, const std::string &message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        clear_config_environment();

        // -------------------------------------------------
        // Test 1: Defaults
        // -------------------------------------------------

        {
            GatewayConfig config = load_config();

            expect(config.port == 8080,
                   "Default gateway port is incorrect");

            expect(config.backends.size() == 3,
                   "Default backend count is incorrect");

            expect(config.backends[0].host == "127.0.0.1" &&
                       config.backends[0].port == 9001,
                   "Default backend 1 is incorrect");

            expect(config.backends[1].host == "127.0.0.1" &&
                       config.backends[1].port == 9002,
                   "Default backend 2 is incorrect");

            expect(config.backends[2].host == "127.0.0.1" &&
                       config.backends[2].port == 9003,
                   "Default backend 3 is incorrect");

            expect(config.rate_limiting_enabled,
                   "Default rate limiting state is incorrect");

            expect(config.rate_limit_capacity == 100000.0,
                   "Default rate limit capacity is incorrect");

            expect(config.rate_limit_refill_rate == 100000.0,
                   "Default rate limit refill rate is incorrect");

            expect(config.redis_host == "127.0.0.1",
                   "Default Redis host is incorrect");

            expect(config.redis_port == "6379",
                   "Default Redis port is incorrect");

            expect(
                config.backend_connect_timeout ==
                    std::chrono::milliseconds(2000),
                "Default backend connect timeout is incorrect");

            expect(
                config.backend_response_timeout ==
                    std::chrono::milliseconds(5000),
                "Default backend response timeout is incorrect");

            expect(
                config.request_timeout ==
                    std::chrono::milliseconds(10000),
                "Default request timeout is incorrect");
        }

        // -------------------------------------------------
        // Test 2: Custom scalar configuration
        // -------------------------------------------------

        {
            set_env("GATEWAY_PORT", "9090");
            set_env("RATE_LIMITING_ENABLED", "false");
            set_env("RATE_LIMIT_CAPACITY", "500");
            set_env("RATE_LIMIT_REFILL_RATE", "25.5");
            set_env("REDIS_HOST", "redis");
            set_env("REDIS_PORT", "6380");

            GatewayConfig config = load_config();

            expect(config.port == 9090,
                   "Custom gateway port is incorrect");

            expect(!config.rate_limiting_enabled,
                   "Custom rate limiting state is incorrect");

            expect(config.rate_limit_capacity == 500.0,
                   "Custom rate limit capacity is incorrect");

            expect(config.rate_limit_refill_rate == 25.5,
                   "Custom rate limit refill rate is incorrect");

            expect(config.redis_host == "redis",
                   "Custom Redis host is incorrect");

            expect(config.redis_port == "6380",
                   "Custom Redis port is incorrect");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 3: Single backend
        // -------------------------------------------------

        {
            set_env("BACKENDS", "10.0.0.1:9005");

            GatewayConfig config = load_config();

            expect(config.backends.size() == 1,
                   "Single backend count is incorrect");

            expect(config.backends[0].host == "10.0.0.1",
                   "Single backend host is incorrect");

            expect(config.backends[0].port == 9005,
                   "Single backend port is incorrect");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 4: Multiple backends
        // -------------------------------------------------

        {
            set_env(
                "BACKENDS",
                "127.0.0.1:9001,"
                "127.0.0.1:9002,"
                "10.0.0.5:9010,"
                "backend.example.com:8080");

            GatewayConfig config = load_config();

            expect(config.backends.size() == 4,
                   "Multiple backend count is incorrect");

            expect(config.backends[0].port == 9001,
                   "Backend 1 port is incorrect");

            expect(config.backends[1].port == 9002,
                   "Backend 2 port is incorrect");

            expect(config.backends[2].host == "10.0.0.5" &&
                       config.backends[2].port == 9010,
                   "Backend 3 is incorrect");

            expect(config.backends[3].host == "backend.example.com" &&
                       config.backends[3].port == 8080,
                   "Backend 4 is incorrect");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 5: Invalid gateway port
        // -------------------------------------------------

        {
            set_env("GATEWAY_PORT", "70000");

            bool threw = false;

            try
            {
                load_config();
            }
            catch (const std::exception &)
            {
                threw = true;
            }

            expect(threw,
                   "Invalid gateway port was not rejected");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 6: Invalid boolean
        // -------------------------------------------------

        {
            set_env("RATE_LIMITING_ENABLED", "maybe");

            bool threw = false;

            try
            {
                load_config();
            }
            catch (const std::exception &)
            {
                threw = true;
            }

            expect(threw,
                   "Invalid boolean was not rejected");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 7: Invalid backend
        // -------------------------------------------------

        {
            set_env("BACKENDS", "127.0.0.1");

            bool threw = false;

            try
            {
                load_config();
            }
            catch (const std::exception &)
            {
                threw = true;
            }

            expect(threw,
                   "Backend without port was not rejected");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 8: Backend port out of range
        // -------------------------------------------------

        {
            set_env("BACKENDS", "127.0.0.1:70000");

            bool threw = false;

            try
            {
                load_config();
            }
            catch (const std::exception &)
            {
                threw = true;
            }

            expect(threw,
                   "Backend port out of range was not rejected");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 9: Custom timeout configuration
        // -------------------------------------------------

        {
            set_env("BACKEND_CONNECT_TIMEOUT_MS", "1500");
            set_env("BACKEND_RESPONSE_TIMEOUT_MS", "7500");
            set_env("REQUEST_TIMEOUT_MS", "12000");

            GatewayConfig config = load_config();

            expect(
                config.backend_connect_timeout ==
                    std::chrono::milliseconds(1500),
                "Custom backend connect timeout is incorrect");

            expect(
                config.backend_response_timeout ==
                    std::chrono::milliseconds(7500),
                "Custom backend response timeout is incorrect");

            expect(
                config.request_timeout ==
                    std::chrono::milliseconds(12000),
                "Custom request timeout is incorrect");

            clear_config_environment();
        }

        // -------------------------------------------------
        // Test 10: Invalid timeout configuration
        // -------------------------------------------------

        {
            set_env("BACKEND_RESPONSE_TIMEOUT_MS", "-500");

            bool threw = false;

            try
            {
                load_config();
            }
            catch (const std::exception &)
            {
                threw = true;
            }

            expect(
                threw,
                "Invalid backend response timeout was not rejected");

            clear_config_environment();
        }

        std::cout << "[PASS] Config tests passed\n";

        return 0;
    }
    catch (const std::exception &e)
    {
        clear_config_environment();

        std::cerr << "[FAIL] " << e.what() << '\n';

        return 1;
    }
}