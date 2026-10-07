#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

enum class LogLevel
{
    INFO,
    WARN,
    ERR
};

class Logger
{
public:
    static void log(
        LogLevel level,
        const std::string &event,
        const std::string &message = "",
        const std::string &request_id = "",
        const std::string &backend = "")
    {
        std::ostream &out =
            level == LogLevel::ERR
                ? std::cerr
                : std::cout;

        out
            << "timestamp=" << timestamp()
            << " level=" << level_to_string(level)
            << " event=" << event;

        if (!request_id.empty())
        {
            out
                << " request_id=" << request_id;
        }

        if (!backend.empty())
        {
            out
                << " backend=" << backend;
        }

        if (!message.empty())
        {
            out
                << " message=\"" << message << "\"";
        }

        out << '\n';
    }

private:
    static std::string timestamp()
    {
        const auto now =
            std::chrono::system_clock::now();

        const auto time =
            std::chrono::system_clock::to_time_t(now);

        std::tm tm{};

#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif

        std::ostringstream oss;

        oss << std::put_time(
            &tm,
            "%Y-%m-%dT%H:%M:%SZ");

        return oss.str();
    }

    static const char *level_to_string(
        LogLevel level)
    {
        switch (level)
        {
        case LogLevel::INFO:
            return "INFO";

        case LogLevel::WARN:
            return "WARN";

        case LogLevel::ERR:
            return "ERROR";
        }

        return "UNKNOWN";
    }
};