# C++ API Gateway --- Remaining Features & Claude Code Plan

## Purpose and working rules

Use this as the working plan for Claude Code in the existing repository.
Inspect the repository before editing; this plan may lag behind the
code.

-   Work incrementally, one focused feature at a time. Do not rewrite
    working code or perform unrelated cleanup.
-   Add tests with every behavior change. Build and run relevant tests,
    then the full test suite.
-   Never claim tests passed unless commands actually passed.
-   Preserve current behavior, especially round-robin as the default,
    health checks, circuit breakers, failover, retry exclusion, Redis
    rate limiting, metrics, request IDs, keep-alive, and endpoints.
-   Before a commit, inspect `git status`, `git diff --check`, and
    `git diff --stat`. Commit only relevant files with a descriptive
    message; push if that is the established workflow.
-   Do not invent benchmark results. Document exact conditions and
    limitations.
-   Do not add TLS, HTTP/2, WebSockets, JWT/authentication, caching,
    service discovery, Kubernetes, or OpenTelemetry unless explicitly
    approved.
-   Explain changed files, design choices, commands, actual test
    results, and proposed commit message concisely.

## Features believed to be implemented (verify in the repository)

-   Async C++20 reverse proxy using Boost.Asio/Beast
-   Multiple backends; round-robin load balancing
-   Persistent backend connections / keep-alive
-   Backend health checks, automatic failover, circuit breaker
-   Redis-backed distributed rate limiting with atomic Lua token bucket
-   Active client-connection metrics
-   Prometheus metrics endpoint `/metrics`, latency histogram,
    P50/P95/P99 Grafana panels
-   Prometheus + Grafana monitoring
-   React/Vite operator dashboard
-   Environment-driven configuration
-   Request/correlation ID (`X-Request-ID`)
-   Structured logging
-   Backend response timeout
-   Retry policy with failed backend excluded from retry selection
-   Integration tests for retry exclusion and timeout failover
-   Unit/integration tests and `wrk` benchmark setup
-   Rate-limit ON/OFF benchmark

The user most recently reported that all tests passed after adding an
atomic `Backend::in_flight_requests` counter, explicit `Backend`
copy/move operations, and a `LoadBalancingStrategy` enum/setter. Verify
`git status` and the source before assuming these changes are committed
or that later changes were made.

## Current task: least-connections strategy

The goal is to make least-connections optional while keeping round-robin
as the default. The strategy enum and setter were added, and existing
tests reportedly passed. Least-connections selection and in-flight
request accounting are NOT confirmed complete. Inspect the current code;
do not assume a suggested helper has been added.

### Requirements

1.  Round-robin behavior and default must remain unchanged.
2.  Least-connections selects the eligible backend with the smallest
    current in-flight request count.
3.  Both strategies respect `Backend::healthy`, circuit-breaker
    eligibility/state transitions, and `excluded_backend` during
    retries.
4.  Preserve `CircuitBreaker::Transition` reporting. Do not call
    `allow_request()` twice for one candidate: it may transition OPEN to
    HALF_OPEN.
5.  Do not use `active_connections` metrics for backend selection; that
    metric counts client connections, not backend in-flight requests.
6.  Track acquire/release for each backend attempt and release exactly
    once on success, DNS/resolve failure, TCP connect failure, request
    write failure, response read failure, timeout, retry, and client
    write failure after the backend response is complete. Prevent
    counter underflow and leaks.
7.  Use an idempotent per-session acquire/release helper or ownership
    flag to track which backend this session currently holds.
8.  Define and test tie-breaking when counts are equal. Avoid
    starvation.
9.  Avoid copying live backends. `Backend`'s atomic counter makes
    implicit copying unavailable; explicit operations currently copy the
    count and circuit-breaker runtime state. Do not redesign this
    broadly without first inspecting call sites.
10. Add strategy configuration only after selection and accounting work.
    Follow existing config conventions and keep round-robin default.

### LC1 --- Selection logic

Inspect `backend.hpp`, `load_balancer.hpp`, `circuit_breaker.hpp`,
`server.hpp`, config files, and current load-balancer tests.

Implement strategy-aware selection with minimal changes. Add unit tests
for: - Default strategy is round-robin. - Round-robin selection order is
unchanged. - Least-connections selects the lowest in-flight count. -
Equal-count tie policy. - Unhealthy backend is skipped. - Open circuit
breaker is skipped and half-open transition is reported. - Excluded
backend is never selected during retry. - No eligible backend throws the
existing expected error.

Do not call `CircuitBreaker::allow_request()` twice per candidate. Build
and run the relevant tests and then the full suite. Commit after tests
pass. Suggested commit:
`feat: add least-connections load balancing strategy`.

### LC2 --- Request accounting

Trace all `Session` paths from backend selection through
resolve/connect/write/read/timeout/retry/client response. Implement
idempotent acquire/release helpers. Release each count exactly once on
every terminal/failure path. Do not conflate backend in-flight requests
with client connection metrics.

Add integration tests proving: - A backend with more in-flight requests
loses selection to a less-loaded eligible backend. - Success releases
the count. - Timeout and retry release the failed attempt and count the
next attempt correctly. - Connection/read/write failures release the
count where testable. - No count leaks or underflow.

Run the full suite and commit. Suggested commit:
`feat: track in-flight backend requests`.

### LC3 --- Strategy configuration

Inspect existing environment/config conventions. If necessary, add an
optional setting for strategy with documented valid values and
round-robin default. Invalid values should be handled consistently with
existing config behavior. Add config tests and README documentation. Run
all tests and commit: `feat: configure load balancing strategy`.

## Remaining production hardening

### Connect timeout and overall request timeout

The current backend response timer starts in `read_from_backend()`,
after connect and request write. It covers waiting for the backend HTTP
response, not DNS, TCP connection establishment, or request writing.

-   Inspect `GatewayConfig`: connect-timeout and overall-request-timeout
    fields may already exist but may not be wired into `Session`.
-   Add a connect timeout around resolve/connect using safe cancellation
    and stale-callback handling.
-   Add an overall request deadline only if it can share clear
    timeout/failure handling.
-   Cancel/invalidate timers on success, retry, and request completion.
-   Add deterministic tests with delayed/unavailable backends;
    distinguish timeout types in logs/metrics.
-   Run full suite. Suggested commits:
    `feat: enforce backend connect timeout` and
    `feat: enforce overall request timeout`.

### Graceful shutdown

-   Inspect acceptor, health-check timer, Redis operations, signal
    handling, and callback lifetimes.
-   Stop accepting new connections on SIGINT/SIGTERM.
-   Cancel recurring health checks and timers.
-   Define whether in-flight requests drain for a bounded grace period
    or close.
-   Avoid use-after-free from callbacks capturing `this`.
-   Test idle shutdown and controlled in-flight shutdown where
    practical.
-   Run full suite. Commit: `feat: add graceful gateway shutdown`.

## Packaging and delivery

### Docker Compose

Create or finish a reproducible local stack containing the gateway,
Redis, at least two sample backends, Prometheus, and Grafana. Include
health checks, environment config, documented ports/startup
dependencies, and no secrets. Validate clean startup, `/metrics`,
dashboards, Redis rate limiting, and failover. Commit:
`build: add full gateway monitoring stack with compose`.

### CI

Use an existing CI provider if present; otherwise ask the owner before
choosing. Build the project and run unit/integration tests with CTest.
Provision Redis/test dependencies or clearly separate network-dependent
tests. Document local commands. Commit:
`ci: build gateway and run tests`.

### Controlled benchmarks

Measure, do not fabricate: - Rate limiting ON vs OFF. - Round-robin vs
least-connections. - Throughput, average latency, P50, P90/P95, P99, max
latency if available, errors and timeouts. - Record OS/machine,
compiler/build type, `wrk` arguments, duration, threads, connections,
backend delays/failure conditions. - Keep conditions identical; repeat
runs where practical. State local results are not production guarantees.

Historical local rate-limit benchmark (context only; rerun before
publishing): - OFF: about 939 req/s; average 55.25 ms; P50 44.37 ms; P90
84.65 ms; P99 233.13 ms. - ON: about 672.34 req/s; average 75.30 ms; P50
61.31 ms; P90 116.43 ms; P99 274.04 ms. - In that local run, ON had
about 28.4% lower throughput, 36.3% higher average latency, 38.2% higher
P50, 37.5% higher P90, and 17.5% higher P99. - These are historical
local measurements, not production overhead claims.

Commit a report only after collecting reproducible data:
`docs: add reproducible gateway benchmark results`.

### README and architecture

Document: - Features and design decisions; Mermaid architecture
diagram. - Build prerequisites and exact build/test commands. -
Environment/config reference. - Compose startup/teardown. - `/metrics`,
`/api/status`, proxy behavior, dashboard, Prometheus/Grafana. -
Load-balancing strategies and default. - Retry policy and retryable
methods. - Health checks, circuit breaker, failover, Redis rate
limiting. - Response timeout vs connect/overall request timeout
semantics. - Benchmark methodology/results and known limitations. -
Never claim unsupported features or performance numbers.

Commit: `docs: document gateway architecture and operations`.

## Final completion checklist

-   [ ] Build works from clean configuration using documented steps.
-   [ ] Full CTest suite passes.
-   [ ] Round-robin remains default and existing behavior is preserved.
-   [ ] Least-connections accounting cannot leak or underflow.
-   [ ] Timeout, retry, failover, circuit breaker, health checks, and
    rate limiting regression tests pass.
-   [ ] Compose stack starts cleanly and documented endpoints work.
-   [ ] CI is green.
-   [ ] Benchmarks are measured, repeatable, and honestly documented.
-   [ ] README/diagram/limitations match the actual implementation.
-   [ ] Milestone commits are clear and pushed if that is the
    established workflow.

## PowerShell commands

Build:

``` powershell
cmake --build build -j1
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
```

Full tests:

``` powershell
ctest --test-dir build --output-on-failure -j1
if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
```

Before commit:

``` powershell
git status
git diff --check
git diff --stat
```

## First task for Claude Code

Start by checking `git status`. Read the current backend, load-balancer
implementation and tests, `server.hpp`, configuration, and
`circuit_breaker.hpp`. Report each roadmap item as complete, partial, or
missing based on actual source. Then implement LC1 only. Do not
implement the entire roadmap in one pass.
