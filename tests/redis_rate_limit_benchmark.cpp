#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "redis_rate_limit_store.hpp"

class BenchmarkState
    : public std::enable_shared_from_this<BenchmarkState>
{
public:
    static constexpr int total_operations = 100000;
    static constexpr int max_concurrency = 100;
    static constexpr int warmup_operations = 100;

    static constexpr double capacity = 100000.0;
    static constexpr double refill_rate = 100000.0;

    BenchmarkState(
        RedisRateLimitStore &store,
        boost::asio::io_context &io_context)
        : store_(store),
          io_context_(io_context)
    {
    }

    void start()
    {
        auto self = shared_from_this();

        boost::asio::post(
            io_context_,
            [self]()
            {
                self->start_warmup();
            });
    }

    bool wait_until_finished(
        std::chrono::seconds timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        return finished_cv_.wait_for(
            lock,
            timeout,
            [this]()
            {
                return finished_;
            });
    }

    int completed() const
    {
        return completed_.load(
            std::memory_order_acquire);
    }

    int allowed() const
    {
        return allowed_.load(
            std::memory_order_acquire);
    }

    double elapsed_seconds() const
    {
        return elapsed_seconds_;
    }

    const std::vector<double> &latencies() const
    {
        return latencies_;
    }

private:
    RedisRateLimitStore &store_;
    boost::asio::io_context &io_context_;

    int warmup_completed_ = 0;
    int submitted_ = 0;
    int outstanding_ = 0;

    std::atomic<int> completed_{0};
    std::atomic<int> allowed_{0};

    bool finished_ = false;

    std::chrono::steady_clock::time_point start_time_;
    double elapsed_seconds_ = 0.0;

    std::vector<double> latencies_;

    mutable std::mutex mutex_;
    std::condition_variable finished_cv_;

    void start_warmup()
    {
        for (int i = 0;
             i < warmup_operations;
             ++i)
        {
            submit_warmup(i);
        }
    }

    void submit_warmup(int operation)
    {
        const std::string client_id =
            "benchmark-warmup-" +
            std::to_string(operation);

        auto self = shared_from_this();

        store_.async_consume(
            client_id,
            capacity,
            refill_rate,
            [self](bool)
            {
                ++self->warmup_completed_;

                if (self->warmup_completed_ ==
                    warmup_operations)
                {
                    self->start_measured();
                }
            });
    }

    void start_measured()
    {
        latencies_.reserve(total_operations);

        /*
         * Start the timer AFTER the warm-up has
         * completed and the Redis connection is active.
         */
        start_time_ =
            std::chrono::steady_clock::now();

        /*
         * Fill the pipeline with exactly
         * max_concurrency operations.
         */
        for (int i = 0;
             i < max_concurrency;
             ++i)
        {
            submit_next();
        }
    }

    void submit_next()
    {
        if (submitted_ >= total_operations)
        {
            return;
        }

        const int operation =
            submitted_++;

        ++outstanding_;

        const auto request_start =
            std::chrono::steady_clock::now();

        const std::string client_id =
            "benchmark-client-" +
            std::to_string(operation % 100);

        auto self = shared_from_this();

        store_.async_consume(
            client_id,
            capacity,
            refill_rate,
            [self, request_start](bool is_allowed)
            {
                const auto request_end =
                    std::chrono::steady_clock::now();

                const double latency_ms =
                    std::chrono::duration<double, std::milli>(
                        request_end - request_start)
                        .count();

                self->latencies_.push_back(
                    latency_ms);

                if (is_allowed)
                {
                    self->allowed_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }

                self->completed_.fetch_add(
                    1,
                    std::memory_order_release);

                --self->outstanding_;

                if (self->completed_.load(
                        std::memory_order_acquire) ==
                    total_operations)
                {
                    self->finish();
                    return;
                }

                self->submit_next();
            });
    }

    void finish()
    {
        const auto end =
            std::chrono::steady_clock::now();

        elapsed_seconds_ =
            std::chrono::duration<double>(
                end - start_time_)
                .count();

        {
            std::lock_guard<std::mutex> lock(
                mutex_);

            finished_ = true;
        }

        finished_cv_.notify_one();
    }
};

int main()
{
    boost::asio::io_context io_context;

    RedisRateLimitStore store(
        io_context,
        "127.0.0.1",
        "6379");

    std::thread io_thread(
        [&]()
        {
            io_context.run();
        });

    auto benchmark =
        std::make_shared<BenchmarkState>(
            store,
            io_context);

    benchmark->start();

    const bool finished =
        benchmark->wait_until_finished(
            std::chrono::seconds(30));

    if (!finished)
    {
        std::cerr
            << "Benchmark timed out.\n";

        boost::asio::post(
            io_context,
            [&store]()
            {
                store.shutdown();
            });

        io_thread.join();

        return 1;
    }

    /*
     * All measured operations have completed.
     *
     * Now stop the long-running Boost.Redis
     * async_run() operation.
     */
    boost::asio::post(
        io_context,
        [&store]()
        {
            store.shutdown();
        });

    io_thread.join();

    const int completed =
        benchmark->completed();

    const int allowed =
        benchmark->allowed();

    const int failed =
        completed - allowed;

    const double elapsed =
        benchmark->elapsed_seconds();

    const auto &latencies =
        benchmark->latencies();

    std::vector<double> sorted_latencies =
        latencies;

    std::sort(
        sorted_latencies.begin(),
        sorted_latencies.end());

    auto percentile =
        [&](double p) -> double
    {
        if (sorted_latencies.empty())
        {
            return 0.0;
        }

        const double index =
            p *
            (sorted_latencies.size() - 1);

        const auto lower =
            static_cast<std::size_t>(
                index);

        const auto upper =
            std::min(
                lower + 1,
                sorted_latencies.size() - 1);

        const double fraction =
            index - lower;

        return sorted_latencies[lower] +
               fraction *
                   (sorted_latencies[upper] -
                    sorted_latencies[lower]);
    };

    const double p50 =
        percentile(0.50);

    const double p95 =
        percentile(0.95);

    const double p99 =
        percentile(0.99);

    const double max_latency =
        sorted_latencies.empty()
            ? 0.0
            : sorted_latencies.back();

    const double throughput =
        completed == BenchmarkState::total_operations
            ? completed / elapsed
            : 0.0;

    std::cout << '\n';

    std::cout
        << "====================================\n";

    std::cout
        << "Redis Rate Limit Benchmark\n";

    std::cout
        << "====================================\n";

    std::cout
        << "Warm-up operations: "
        << BenchmarkState::warmup_operations
        << '\n';

    std::cout
        << "Measured operations: "
        << BenchmarkState::total_operations
        << '\n';

    std::cout
        << "Max concurrency: "
        << BenchmarkState::max_concurrency
        << '\n';

    std::cout
        << "Completed: "
        << completed
        << '\n';

    std::cout
        << "Allowed: "
        << allowed
        << '\n';

    std::cout
        << "Rejected/errors: "
        << failed
        << '\n';

    std::cout
        << std::fixed
        << std::setprecision(3);

    std::cout
        << "Elapsed: "
        << elapsed
        << " s\n";

    std::cout
        << "Throughput: "
        << throughput
        << " ops/sec\n";

    std::cout
        << "P50 latency: "
        << p50
        << " ms\n";

    std::cout
        << "P95 latency: "
        << percentile(0.95)
        << " ms\n";

    std::cout
        << "P99 latency: "
        << p99
        << " ms\n";

    std::cout
        << "Max latency: "
        << max_latency
        << " ms\n";

    std::cout
        << "====================================\n";

    return 0;
}