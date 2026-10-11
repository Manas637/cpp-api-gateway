#!/usr/bin/env bash
#
# bench_case.sh - run ONE controlled wrk load against the gateway (or a
# backend), capturing gateway metrics before/after so the client-visible
# result can be reconciled with server-side counters.
#
# Guarantees:
#   * Never tears down a Compose stack and never removes volumes. The
#     caller decides what is running. (No `docker compose down -v`.)
#   * Writes every artifact into a fresh timestamped directory and never
#     overwrites existing results.
#   * Verifies the actual container and network names at runtime.
#
# Metric semantics relied on for the derived section (see src/metrics.cpp):
#   * record_response() buckets 2xx/4xx/5xx by class only. The gateway emits
#     exactly 200, 429, 503 and 504 (verified against src/server.hpp), never
#     500/502.
#   * rate_limit_rejected_total is incremented once per rejected request and
#     is always accompanied by a 429 response -> 429 == 4xx class here.
#   * request_timeouts_total is incremented once per overall request timeout
#     and is always accompanied by a 504 response -> 504 == request_timeouts.
#   * Therefore the ONLY 5xx statuses are 503 and 504, so:
#         derived_503 = responses_5xx_delta - request_timeouts_delta
#     This is a DERIVED value that holds only while the metric semantics above
#     remain true. It is labelled as derived, never as a measured count.
#   * backend_failures_total counts failure EVENTS, not client-visible 503s:
#     a single request may retry once, producing up to two failure events.
#     It must not be treated as an exact 503 count.
#   * Counters are cumulative. A restart resets them; a negative delta is
#     reported as N/A rather than wrapped around.
#
set -euo pipefail

# ---------------------------------------------------------------- inputs ---
LABEL=${LABEL:-case}
DUR=${DUR:-30s}
THREADS=${THREADS:-4}
CONNS=${CONNS:-100}
TARGET_URL=${TARGET_URL:-http://gateway:8080/}
CONN_HEADER=${CONN_HEADER:-}              # e.g. "Connection: close"; empty = keep-alive
WRK_IMAGE=${WRK_IMAGE:-williamyeh/wrk}
OUTROOT=${OUTROOT:-bench_results}
RUN_ID=${RUN_ID:-$(date +%Y%m%d-%H%M%S)}

# Discovery inputs (empty => auto-resolve from Docker).
GATEWAY_CONTAINER=${GATEWAY_CONTAINER:-}
GATEWAY_PORT=${GATEWAY_PORT:-8080}
NETWORK=${NETWORK:-}

# Gateway metrics are only meaningful when the load target is the gateway.
CAPTURE_METRICS=${CAPTURE_METRICS:-1}

# Provenance inputs (empty => auto-detect from Docker, else record "unknown").
#   COMPOSE_FILE      exact Compose file used for the running stack
#   BACKEND_MODE      backend implementation/concurrency model
#   BACKEND_CONTAINER backend container to describe
COMPOSE_FILE=${COMPOSE_FILE:-}
BACKEND_MODE=${BACKEND_MODE:-}
BACKEND_CONTAINER=${BACKEND_CONTAINER:-}

# Provenance inputs (empty => auto-detect from Docker, else record "unknown").
#   COMPOSE_FILE      exact Compose file used for the running stack
#   BACKEND_MODE      backend implementation/concurrency model, e.g. "threaded"
#                     or "asyncio"
#   BACKEND_CONTAINER backend container to describe (auto-detects backend-[1-2]/backend-[a-b])
COMPOSE_FILE=${COMPOSE_FILE:-}
BACKEND_MODE=${BACKEND_MODE:-}
BACKEND_CONTAINER=${BACKEND_CONTAINER:-}

# ------------------------------------------------------------ discovery ----
if [ -z "$GATEWAY_CONTAINER" ]; then
  GATEWAY_CONTAINER=$(docker ps \
    --filter "label=com.docker.compose.service=gateway" \
    --format '{{.Names}}' | head -n1)
fi
if [ -z "$GATEWAY_CONTAINER" ]; then
  echo "ERROR: could not resolve the gateway container." >&2
  echo "       Set GATEWAY_CONTAINER=... explicitly." >&2
  exit 1
fi

if ! docker inspect "$GATEWAY_CONTAINER" >/dev/null 2>&1; then
  echo "ERROR: gateway container '$GATEWAY_CONTAINER' does not exist." >&2
  exit 1
fi

if [ -z "$NETWORK" ]; then
  NETWORK=$(docker inspect "$GATEWAY_CONTAINER" \
    --format '{{range $k,$v := .NetworkSettings.Networks}}{{$k}}{{end}}')
fi
if [ -z "$NETWORK" ]; then
  echo "ERROR: could not resolve the gateway network." >&2
  exit 1
fi

# --------------------------------------------------------- provenance ------
# Record enough context to reproduce and interpret the run. Anything that
# cannot be determined is recorded as "unknown" rather than guessed, so that
# historical runs are never given invented metadata.
COMPOSE_PROJECT=$(docker inspect "$GATEWAY_CONTAINER" \
  --format '{{index .Config.Labels "com.docker.compose.project"}}' 2>/dev/null || echo "")
COMPOSE_FILE_LABEL=$(docker inspect "$GATEWAY_CONTAINER" \
  --format '{{index .Config.Labels "com.docker.compose.project.config_files"}}' 2>/dev/null || echo "")
COMPOSE_FILE=${COMPOSE_FILE:-${COMPOSE_FILE_LABEL:-unknown}}
[ -z "$COMPOSE_PROJECT" ] && COMPOSE_PROJECT=unknown

if [ -z "$BACKEND_CONTAINER" ]; then
  BACKEND_CONTAINER=$(docker ps \
    --filter "label=com.docker.compose.service=backend-1" \
    --format '{{.Names}}' | head -n1)
fi
if [ -n "$BACKEND_CONTAINER" ] && docker inspect "$BACKEND_CONTAINER" >/dev/null 2>&1; then
  BACKEND_IMAGE=$(docker inspect "$BACKEND_CONTAINER" \
    --format '{{.Config.Image}}' 2>/dev/null || echo unknown)
  BACKEND_CMD=$(docker inspect "$BACKEND_CONTAINER" \
    --format '{{join .Config.Cmd " "}}' 2>/dev/null || echo unknown)
else
  BACKEND_CONTAINER=unknown
  BACKEND_IMAGE=unknown
  BACKEND_CMD=unknown
fi
if [ -z "$BACKEND_MODE" ]; then
  MODE_TOKEN=$(printf '%s\n' "$BACKEND_CMD" \
    | awk '{for(i=1;i<=NF;i++) if($i=="--mode"){print $(i+1); exit}}')
  if [ -n "$MODE_TOKEN" ]; then
    BACKEND_MODE="$MODE_TOKEN"
  else
    case "$BACKEND_IMAGE" in
      *research*) BACKEND_MODE="threaded (research_backend.py default)" ;;
      *backend*)  BACKEND_MODE="threaded (backend.py)" ;;
      *)          BACKEND_MODE="unknown" ;;
    esac
  fi
fi

WRK_IMAGE_ID=$(docker image inspect "$WRK_IMAGE" --format '{{.Id}}' 2>/dev/null || echo unknown)

# Gateway configuration that affects how a result should be interpreted.
gateway_env() {
  docker exec "$GATEWAY_CONTAINER" printenv "$1" 2>/dev/null || echo unknown
}
RL_CAPACITY=$(gateway_env RATE_LIMIT_CAPACITY)
RL_REFILL=$(gateway_env RATE_LIMIT_REFILL_RATE)
BC_TIMEOUT=$(gateway_env BACKEND_CONNECT_TIMEOUT_MS)
BR_TIMEOUT=$(gateway_env BACKEND_RESPONSE_TIMEOUT_MS)
REQ_TIMEOUT=$(gateway_env REQUEST_TIMEOUT_MS)


# --------------------------------------------------------- provenance ------
# Record enough context to reproduce and interpret the run. Anything that
# cannot be determined is recorded as "unknown" rather than guessed.
COMPOSE_PROJECT=$(docker inspect "$GATEWAY_CONTAINER"   --format '{{index .Config.Labels "com.docker.compose.project"}}' 2>/dev/null || echo "")
COMPOSE_FILE_LABEL=$(docker inspect "$GATEWAY_CONTAINER"   --format '{{index .Config.Labels "com.docker.compose.project.config_files"}}' 2>/dev/null || echo "")
COMPOSE_FILE=${COMPOSE_FILE:-${COMPOSE_FILE_LABEL:-unknown}}
[ -z "$COMPOSE_PROJECT" ] && COMPOSE_PROJECT=unknown

if [ -z "$BACKEND_CONTAINER" ]; then
  for svc in backend-1 backend-2 backend-a backend-b; do
    BACKEND_CONTAINER=$(docker ps       --filter "label=com.docker.compose.service=$svc"       --format '{{.Names}}' | head -n1)
    [ -n "$BACKEND_CONTAINER" ] && break
  done
  # fallback by name pattern
  if [ -z "$BACKEND_CONTAINER" ]; then
    BACKEND_CONTAINER=$(docker ps --format '{{.Names}}' | grep -E '^cpp-api-gateway-backend-[a-zA-Z0-9]+' | head -n1)
  fi
fi
if [ -n "$BACKEND_CONTAINER" ] && docker inspect "$BACKEND_CONTAINER" >/dev/null 2>&1; then
  BACKEND_IMAGE=$(docker inspect "$BACKEND_CONTAINER"     --format '{{.Config.Image}}' 2>/dev/null || echo unknown)
  BACKEND_CMD=$(docker inspect "$BACKEND_CONTAINER"     --format '{{join .Config.Cmd " "}}' 2>/dev/null || echo unknown)
else
  BACKEND_CONTAINER=unknown
  BACKEND_IMAGE=unknown
  BACKEND_CMD=unknown
fi
if [ -z "$BACKEND_MODE" ]; then
  MODE_TOKEN=$(printf '%s
' "$BACKEND_CMD"     | awk '{for(i=1;i<=NF;i++) if($i=="--mode"){print $(i+1); exit}}')
  if [ -n "$MODE_TOKEN" ]; then
    BACKEND_MODE="$MODE_TOKEN"
  else
    case "$BACKEND_IMAGE" in
      *research*) BACKEND_MODE="threaded (research_backend.py default)" ;;
      *backend*)  BACKEND_MODE="threaded (backend.py)" ;;
      *)          BACKEND_MODE="unknown" ;;
    esac
  fi
fi

WRK_IMAGE_ID=$(docker image inspect "$WRK_IMAGE" --format '{{.Id}}' 2>/dev/null || echo unknown)

gateway_env() {
  docker exec "$GATEWAY_CONTAINER" printenv "$1" 2>/dev/null || echo unknown
}
RL_CAPACITY=$(gateway_env RATE_LIMIT_CAPACITY)
RL_REFILL=$(gateway_env RATE_LIMIT_REFILL_RATE)
BC_TIMEOUT=$(gateway_env BACKEND_CONNECT_TIMEOUT_MS)
BR_TIMEOUT=$(gateway_env BACKEND_RESPONSE_TIMEOUT_MS)
REQ_TIMEOUT=$(gateway_env REQUEST_TIMEOUT_MS)
# ---------------------------------------------------------- output setup ---
OUTDIR="$OUTROOT/${RUN_ID}-${LABEL}"
if [ -e "$OUTDIR" ]; then
  echo "ERROR: output directory already exists: $OUTDIR" >&2
  exit 1
fi
mkdir -p "$OUTDIR"

# ------------------------------------------------------ health prechecks ---
wait_healthy() {
  local container=$1 tries=${2:-60}
  for _ in $(seq 1 "$tries"); do
    local st
    st=$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' \
      "$container" 2>/dev/null || echo missing)
    [ "$st" = "healthy" ] && return 0
    [ "$st" = "none" ] && return 0
    sleep 2
  done
  return 1
}

if ! wait_healthy "$GATEWAY_CONTAINER"; then
  echo "ERROR: gateway '$GATEWAY_CONTAINER' not healthy." >&2
  docker ps -a --filter "name=$GATEWAY_CONTAINER" >&2 || true
  exit 1
fi

# --------------------------------------------------------- metrics io ------
capture_metrics() {
  # $1 = destination file
  if [ "$CAPTURE_METRICS" != "1" ]; then
    return 0
  fi
  docker exec "$GATEWAY_CONTAINER" \
    curl -fsS "http://127.0.0.1:${GATEWAY_PORT}/metrics" > "$1"
}

# ------------------------------------------------------------- run wrk -----
wrk_args=(-t"$THREADS" -c"$CONNS" -d"$DUR" --latency)
if [ -n "$CONN_HEADER" ]; then
  wrk_args+=(-H "$CONN_HEADER")
fi

# -------------------------------------------------------------- metadata ---
TS_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)

CURRENT_STRATEGY=$(docker exec "$GATEWAY_CONTAINER" printenv LOAD_BALANCING_STRATEGY 2>/dev/null || echo unknown)
CURRENT_RL=$(docker exec "$GATEWAY_CONTAINER" printenv RATE_LIMITING_ENABLED 2>/dev/null || echo unknown)

{
  echo "label=$LABEL"
  echo "run_id=$RUN_ID"
  echo "timestamp_utc=$TS_UTC"
  echo "target_url=$TARGET_URL"
  echo "wrk_image=$WRK_IMAGE"
  echo "threads=$THREADS"
  echo "connections=$CONNS"
  echo "duration=$DUR"
  echo "connection_header=${CONN_HEADER:-<none=keep-alive>}"
  echo "network=$NETWORK"
  echo "gateway_container=$GATEWAY_CONTAINER"
  echo "gateway_port=$GATEWAY_PORT"
  echo "compose_project=$COMPOSE_PROJECT"
  echo "compose_file=$COMPOSE_FILE"
  echo "backend_container=$BACKEND_CONTAINER"
  echo "backend_image=$BACKEND_IMAGE"
  echo "backend_command=$BACKEND_CMD"
  echo "backend_mode=$BACKEND_MODE"
  # NOTE: the following gateway fields describe the gateway container and are
  # only meaningful for interpretation when target_url points at the gateway.
  # They are recorded even for direct-backend runs, where they are incidental.
  echo "load_balancing_strategy=$CURRENT_STRATEGY"
  echo "rate_limiting_enabled=$CURRENT_RL"
  echo "rate_limit_capacity=$RL_CAPACITY"
  echo "rate_limit_refill_rate=$RL_REFILL"
  echo "backend_connect_timeout_ms=$BC_TIMEOUT"
  echo "backend_response_timeout_ms=$BR_TIMEOUT"
  echo "request_timeout_ms=$REQ_TIMEOUT"
  echo "wrk_image_id=$WRK_IMAGE_ID"
  echo "compose_project=$COMPOSE_PROJECT"
  echo "compose_file=$COMPOSE_FILE"
  echo "backend_container=$BACKEND_CONTAINER"
  echo "backend_image=$BACKEND_IMAGE"
  echo "backend_command=$BACKEND_CMD"
  echo "backend_mode=$BACKEND_MODE"
  echo "load_balancing_strategy=$CURRENT_STRATEGY"
  echo "rate_limiting_enabled=$CURRENT_RL"
  echo "rate_limit_capacity=$RL_CAPACITY"
  echo "rate_limit_refill_rate=$RL_REFILL"
  echo "backend_connect_timeout_ms=$BC_TIMEOUT"
  echo "backend_response_timeout_ms=$BR_TIMEOUT"
  echo "request_timeout_ms=$REQ_TIMEOUT"
  echo "wrk_command=docker run --rm --network $NETWORK $WRK_IMAGE ${wrk_args[*]} $TARGET_URL"
} > "$OUTDIR/metadata.txt"

# ----------------------------------------------------------- execute -------
echo "== capturing metrics before =="
capture_metrics "$OUTDIR/metrics_before.prom"

echo "== running wrk =="
set +e
docker run --rm --network "$NETWORK" "$WRK_IMAGE" \
  "${wrk_args[@]}" "$TARGET_URL" > "$OUTDIR/wrk.txt" 2>&1
WRK_RC=$?
set -e
echo "wrk_exit_code=$WRK_RC" >> "$OUTDIR/metadata.txt"

echo "== capturing metrics after =="
capture_metrics "$OUTDIR/metrics_after.prom"

# ---------------------------------------------------- wrk result digest ----
if [ -s "$OUTDIR/wrk.txt" ]; then
  awk '
    BEGIN                 { cerr=0; rerr=0; werr=0; terr=0; non2xx=0 }
    /Requests\/sec:/      { rps=$2 }
    /^    Latency[ ]/     { lat_avg=$2; lat_stdev=$3; lat_max=$4 }
    /^     50%/           { p50=$2 }
    /^     75%/           { p75=$2 }
    /^     90%/           { p90=$2 }
    /^     99%/           { p99=$2 }
    /requests in/         { reqs=$1 }
    /Socket errors:/      { cerr=$4; gsub(",","",cerr); rerr=$6; gsub(",","",rerr); werr=$8; gsub(",","",werr); terr=$10 }
    /Non-2xx or 3xx/      { non2xx=$5 }
    END {
      printf "wrk_requests=%s\n", (reqs=="" ? "N/A" : reqs);
      printf "wrk_requests_per_sec=%s\n", (rps=="" ? "N/A" : rps);
      printf "wrk_latency_avg=%s\n", (lat_avg=="" ? "N/A" : lat_avg);
      printf "wrk_latency_stdev=%s\n", (lat_stdev=="" ? "N/A" : lat_stdev);
      printf "wrk_latency_max=%s\n", (lat_max=="" ? "N/A" : lat_max);
      printf "wrk_p50=%s\n", (p50=="" ? "N/A" : p50);
      printf "wrk_p75=%s\n", (p75=="" ? "N/A" : p75);
      printf "wrk_p90=%s\n", (p90=="" ? "N/A" : p90);
      printf "wrk_p99=%s\n", (p99=="" ? "N/A" : p99);
      printf "wrk_connect_errors=%s\n", (cerr=="" ? "N/A" : cerr);
      printf "wrk_read_errors=%s\n", (rerr=="" ? "N/A" : rerr);
      printf "wrk_write_errors=%s\n", (werr=="" ? "N/A" : werr);
      printf "wrk_timeouts=%s\n", (terr=="" ? "N/A" : terr);
      printf "wrk_non2xx=%s\n", (non2xx=="" ? "N/A" : non2xx);
    }
  ' "$OUTDIR/wrk.txt" > "$OUTDIR/wrk_digest.txt"
fi

# ------------------------------------------------------- metric deltas -----
# Extract a single gateway_* counter value from a Prometheus text file.
metric_value() {
  local file=$1 name=$2
  awk -v n="$name" '
    $1 == n { print $2; found=1; exit }
    END { if (!found) print "N/A" }
  ' "$file"
}

write_deltas() {
  local name
  local keys=(
    gateway_requests_total
    gateway_responses_2xx_total
    gateway_responses_4xx_total
    gateway_responses_5xx_total
    gateway_backend_requests_total
    gateway_backend_successes_total
    gateway_backend_failures_total
    gateway_failovers_total
    gateway_request_timeouts_total
    gateway_rate_limit_allowed_total
    gateway_rate_limit_rejected_total
    gateway_circuit_opens_total
  )

  {
    echo "# Server-side metric deltas for run $RUN_ID ($LABEL)"
    echo "# delta = after - before (cumulative counters). N/A = counter reset or unavailable."
    echo "metric,before,after,delta"
  } > "$OUTDIR/deltas.csv"

  for name in "${keys[@]}"; do
    local b a d
    b=$(metric_value "$OUTDIR/metrics_before.prom" "$name")
    a=$(metric_value "$OUTDIR/metrics_after.prom" "$name")
    if [ "$b" = "N/A" ] || [ "$a" = "N/A" ]; then
      d="N/A"
    elif [ "$a" -lt "$b" ]; then
      d="N/A(reset)"
    else
      d=$(( a - b ))
    fi
    printf '%s,%s,%s,%s\n' "$name" "$b" "$a" "$d" >> "$OUTDIR/deltas.csv"
  done

  # ---- human-readable summary + explicitly DERIVED status estimates ----
  # Read deltas back from the csv (authoritative).
  csv_delta() { awk -F, -v n="$1" '$1==n{print $4}' "$OUTDIR/deltas.csv"; }

  {
    echo "== measured metric deltas (not derived) =="
    echo "requests_received            = $(csv_delta gateway_requests_total)"
    echo "responses_2xx                = $(csv_delta gateway_responses_2xx_total)"
    echo "responses_4xx                = $(csv_delta gateway_responses_4xx_total)"
    echo "responses_5xx                = $(csv_delta gateway_responses_5xx_total)"
    echo "request_timeouts             = $(csv_delta gateway_request_timeouts_total)"
    echo "rate_limit_rejected          = $(csv_delta gateway_rate_limit_rejected_total)"
    echo "rate_limit_allowed           = $(csv_delta gateway_rate_limit_allowed_total)"
    echo "backend_requests             = $(csv_delta gateway_backend_requests_total)"
    echo "backend_successes            = $(csv_delta gateway_backend_successes_total)"
    echo "backend_failures (events)    = $(csv_delta gateway_backend_failures_total)"
    echo "failovers                    = $(csv_delta gateway_failovers_total)"
    echo "circuit_opens                = $(csv_delta gateway_circuit_opens_total)"
    echo ""
    echo "== DERIVED status estimates (valid only under metrics.cpp semantics) =="
    echo "429 (~= 4xx class, and ~= rate_limit_rejected)          = $(csv_delta gateway_responses_4xx_total)"
    echo "504 (~= request_timeouts)                               = $(csv_delta gateway_request_timeouts_total)"
    # 503 = 5xx - 504, only if both deltas are numeric.
    local d5 dt derived503
    d5=$(csv_delta gateway_responses_5xx_total)
    dt=$(csv_delta gateway_request_timeouts_total)
    if [[ "$d5" =~ ^[0-9]+$ ]] && [[ "$dt" =~ ^[0-9]+$ ]] && [ "$d5" -ge "$dt" ]; then
      derived503=$(( d5 - dt ))
      echo "503 (DERIVED = 5xx - request_timeouts)                  = $derived503"
    else
      echo "503 (DERIVED = 5xx - request_timeouts)                  = N/A (see deltas.csv)"
    fi
    echo ""
    echo "NOTE: 429/504 are exact because the gateway emits them 1:1 with"
    echo "      rate_limit_rejected / request_timeouts. 503 is DERIVED and"
    echo "      relies on the gateway emitting only 200/429/503/504."
    echo "NOTE: backend_failures counts failure events, not 503 responses;"
    echo "      one request may retry once and add two events. Do not equate."
    echo "NOTE: wrk 'timeouts' are client-side socket timeouts (wrk default 2s)"
    echo "      and are separate from the gateway's request_timeouts (504)."
  } > "$OUTDIR/deltas_summary.txt"
}

if [ "$CAPTURE_METRICS" = "1" ]; then
  write_deltas
fi

echo "== done =="
echo "$OUTDIR"
