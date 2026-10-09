cmake_minimum_required(VERSION 3.20)

project(cpp_api_gateway
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)


# =========================
# API Gateway
# =========================

add_executable(api_gateway
    src/main.cpp
    src/rate_limiter.cpp
    src/redis_rate_limit_store.cpp
    src/metrics.cpp
    src/config.cpp
)

target_include_directories(api_gateway
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(api_gateway
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)


# =========================
# Tests
# =========================

enable_testing()


# -------------------------
# Load Balancer Test
# -------------------------

add_executable(load_balancer_test
    tests/load_balancer_test.cpp
)

target_include_directories(load_balancer_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME LoadBalancerTest
    COMMAND load_balancer_test
)


# -------------------------
# In-Memory Rate Limiter Test
# -------------------------

add_executable(rate_limiter_test
    tests/rate_limiter_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
)

target_include_directories(rate_limiter_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME RateLimiterTest
    COMMAND rate_limiter_test
)


# -------------------------
# In-Memory Rate Limiter
# Concurrency Test
# -------------------------

add_executable(rate_limiter_concurrency_test
    tests/rate_limiter_concurrency_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
)

target_include_directories(rate_limiter_concurrency_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME RateLimiterConcurrencyTest
    COMMAND rate_limiter_concurrency_test
)


# -------------------------
# Redis Connection Test
# -------------------------

add_executable(redis_connection_test
    tests/redis_connection_test.cpp
)

target_include_directories(redis_connection_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_connection_test
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME RedisConnectionTest
    COMMAND redis_connection_test
)


# -------------------------
# Redis Rate Limit Store Test
# -------------------------

add_executable(redis_rate_limit_store_test
    tests/redis_rate_limit_store_test.cpp
    src/rate_limiter.cpp
    src/redis_rate_limit_store.cpp
)

target_include_directories(redis_rate_limit_store_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_rate_limit_store_test
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME RedisRateLimitStoreTest
    COMMAND redis_rate_limit_store_test
)


# -------------------------
# Redis Rate Limiter
# Concurrency Test
# -------------------------

add_executable(redis_rate_limit_concurrency_test
    tests/redis_rate_limit_concurrency_test.cpp
    src/rate_limiter.cpp
    src/redis_rate_limit_store.cpp
)

target_include_directories(redis_rate_limit_concurrency_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_rate_limit_concurrency_test
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME RedisRateLimitConcurrencyTest
    COMMAND redis_rate_limit_concurrency_test
)

add_executable(redis_rate_limit_benchmark
    tests/redis_rate_limit_benchmark.cpp
    src/redis_rate_limit_store.cpp
)

target_include_directories(redis_rate_limit_benchmark
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(redis_rate_limit_benchmark
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_executable(circuit_breaker_test
    tests/circuit_breaker_test.cpp
)

target_include_directories(circuit_breaker_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME CircuitBreakerTest
    COMMAND circuit_breaker_test
)

add_executable(load_balancer_circuit_breaker_test
    tests/load_balancer_circuit_breaker_test.cpp
)

target_include_directories(load_balancer_circuit_breaker_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME LoadBalancerCircuitBreakerTest
    COMMAND load_balancer_circuit_breaker_test
)

add_executable(gateway_integration_test
    tests/gateway_integration_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
    src/metrics.cpp
)

target_include_directories(gateway_integration_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(gateway_integration_test
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME GatewayIntegrationTest
    COMMAND gateway_integration_test
)

# -------------------------
# Session Teardown
# Accounting Test
# -------------------------

add_executable(session_teardown_test
    tests/session_teardown_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
    src/metrics.cpp
)

target_include_directories(session_teardown_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(session_teardown_test
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME SessionTeardownTest
    COMMAND session_teardown_test
)

# -------------------------
# Rate Limit Metrics
# Integration Test
# -------------------------

add_executable(rate_limit_metrics_integration_test
    tests/rate_limit_metrics_integration_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
    src/metrics.cpp
)

target_include_directories(rate_limit_metrics_integration_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(rate_limit_metrics_integration_test
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME RateLimitMetricsIntegrationTest
    COMMAND rate_limit_metrics_integration_test
)

add_executable(metrics_test
    tests/metrics_test.cpp
    src/metrics.cpp
)

target_include_directories(
    metrics_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME MetricsTest
    COMMAND metrics_test
)

# -------------------------
# Prometheus Metrics Test
# -------------------------

add_executable(metrics_prometheus_test
    tests/metrics_prometheus_test.cpp
    src/metrics.cpp
)

target_include_directories(
    metrics_prometheus_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME MetricsPrometheusTest
    COMMAND metrics_prometheus_test
)

add_executable(metrics_endpoint_integration_test
    tests/metrics_endpoint_integration_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
    src/metrics.cpp
)

target_include_directories(metrics_endpoint_integration_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(metrics_endpoint_integration_test
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME MetricsEndpointIntegrationTest
    COMMAND metrics_endpoint_integration_test
)

add_executable(
    ActiveConnectionsIntegrationTest
    tests/active_connections_integration_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
    src/metrics.cpp
)

target_include_directories(
    ActiveConnectionsIntegrationTest
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
    ActiveConnectionsIntegrationTest
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME ActiveConnectionsIntegrationTest
    COMMAND ActiveConnectionsIntegrationTest
)

# -------------------------
# Status Endpoint
# Integration Test
# -------------------------

add_executable(
    status_endpoint_integration_test
    tests/status_endpoint_integration_test.cpp
    src/rate_limiter.cpp
    src/in_memory_rate_limit_store.cpp
    src/metrics.cpp
)

target_include_directories(
    status_endpoint_integration_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(
    status_endpoint_integration_test
    PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32
    mswsock
)

add_test(
    NAME StatusEndpointIntegrationTest
    COMMAND status_endpoint_integration_test
)

# -------------------------
# Configuration Test
# -------------------------

add_executable(
    config_test
    tests/config_test.cpp
    src/config.cpp
)

target_include_directories(
    config_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME ConfigTest
    COMMAND config_test
)