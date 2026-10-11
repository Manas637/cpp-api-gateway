# Controlled benchmark results

Environment:
- OS: Windows (Docker host)
- Build type: Release
- Gateway: C++20 async API gateway (dockerized)
- Backends: 2 identical sample Python backends (same workload)
- Load generator: wrk 4.2.0 (via williamyeh/wrk in Docker)
- Network: Docker bridge on host
- Host CPU/RAM: unknown (not recorded)
- Gateway image digest / compiler version: unknown (not recorded)
- wrk image tag/digest: unknown (not recorded)
- Host CPU/RAM: unknown (not recorded)
- Gateway image digest / compiler version: unknown (not recorded)
- wrk image tag/digest: unknown (not recorded)

Status: these four runs predate the controlled Stage 2/3 diagnostics. The
measurements below are historical and are kept verbatim; the interpretation
notes have been corrected. Some provenance fields were never recorded.

Test parameters (identical within each comparison):
- Duration: 10s
- Threads: 4
- Connections: 100
- Target: /
- Connection close per request

Settings provenance: the duration/threads/connections above (10s / 4 / 100)
reflect what these four runs recorded. They do NOT match the current
defaults in `run_bench_proper.sh` (15s / 4 / 200). The exact command that
produced these files was not preserved, so any generating flag not listed
here should be treated as unknown.

Files:
- bench_results/wrk_1791660115.txt
- bench_results/wrk_1791660204.txt
- bench_results/wrk_1791660314.txt
- bench_results/wrk_1791660397.txt

## Results

### Rate limiting ON vs OFF (Round-robin)
- RR + RL OFF (wrk_1791660115.txt): Requests/sec ~192, Avg latency ~466ms, P50 ~297ms, P90 ~1.24s, P99 ~1.58s, timeouts 40, non-2xx 462 (of ~1935)
- RR + RL ON  (wrk_1791660204.txt): Requests/sec ~167, Avg latency ~512ms, P50 ~384ms, P90 ~1.29s, P99 ~1.81s, timeouts 78, non-2xx 448 (of ~1681)

### Least-connections vs Round-robin (RL OFF)
- LC + RL OFF (wrk_1791660314.txt): Requests/sec ~220, Avg latency ~429ms, P50 ~259ms, P90 ~1.20s, P99 ~1.52s, timeouts 63, non-2xx 557 (of ~2224)
- LC + RL ON  (wrk_1791660397.txt): Requests/sec ~164, Avg latency ~540ms, P50 ~313ms, P90 ~1.30s, P99 ~1.80s, timeouts 57, non-2xx 441 (of ~1658)

Notes:
- Correction: the earlier claim that enabling rate limiting increased
  throttling and produced 429 responses is NOT supported and has been
  removed. Controlled Stage 2/3 diagnostics observed ZERO 429 responses
  (`gateway_rate_limit_rejected_total` never incremented; the configured
  rate-limit capacity/refill were far above the offered load). The per-run
  rate-limit configuration for these historical files was not recorded and
  is unknown.
- The non-2xx counts above are raw `wrk` totals only. `wrk` does not break
  the count down by status code, so these files cannot establish how many,
  if any, non-2xx were 429. In the controlled diagnostics every non-2xx was
  a 503 (no 429, no 504), which is more consistent with backend/connect
  failures than with throttling. This is not proven for these historical
  runs.
- With identical backends, LC vs RR shows small differences in this short test (both strategies preserved; LC slightly higher RPS in RL OFF case here).
- Any RL OFF vs RL ON throughput difference here is dominated by run-to-run
  variance in this single-host Docker environment and must not be read as a
  rate-limiting overhead measurement.

- Stage 4 (Local validation, Windows/Docker Desktop, 8 CPU/3.43 GiB, network cpp-api-gateway_gateway, wrk 4.2.0 williamyeh/wrk): repeated comparisons (30s, t4, c100, RR, RL ON). Threaded keep-alive (3 reps): 1,183–1,393 RPS, P50 61–74 ms, P90 106–137 ms, P99 766–1,100 ms, 0 non2xx. Asyncio keep-alive (3 reps): 3,379–3,615 RPS, P50 22–25 ms, P90 51–54 ms, P99 710–1,190 ms, 0 non2xx. Threaded Connection: close (3 reps): 331–515 RPS, P50 68–124 ms, P90 917–1,090 ms, P99 1,290–1,390 ms, with 2,011–2,146 5xx responses and elevated backend_failures/failovers/circuit opens. Asyncio Connection: close (3 reps): 338–518 RPS, P50 157–236 ms, P90 318–565 ms, P99 1,030–1,630 ms, with zero observed non-2xx responses or backend failures. Variability is non-trivial (single-host). Connection: close consistently shows substantially worse tail latency than keep-alive in both backends. Local artifact naming: s4-* directories preserved (s4-s4-async-gw-*/s4-s4-threaded-gw-*).
- Regression: 15 non-Redis tests passed; 3 Redis-specific tests (RedisConnectionTest, RedisRateLimitStoreTest, RedisRateLimitConcurrencyTest) were skipped because Redis was unavailable at their expected localhost endpoint in the test environment (Redis container runs on internal network without published localhost port). No test failures observed.
