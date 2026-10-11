#!/bin/bash
set -e
# Benchmark parameters. These defaults (15s, 4, 200) differ from the
# historical settings recorded in bench_results/SUMMARY.md (10s, 4, 100);
# the exact command that produced those historical results was not
# preserved. All values are overridable via environment variables.
DUR=${DUR:-15s}
THREADS=${THREADS:-4}
CONNS=${CONNS:-200}
COMPOSE_FILE=${COMPOSE_FILE:-docker-compose.yml}
OUTDIR=${OUTDIR:-bench_results}
mkdir -p "$OUTDIR"

docker compose -f "$COMPOSE_FILE" up -d --build
# wait healthy
for i in $(seq 1 90); do
  st=$(docker inspect --format '{{.State.Health.Status}}' cpp-api-gateway 2>/dev/null || echo "missing")
  [ "$st" = "healthy" ] && break
  sleep 2
done

TS=$(date +%s)
docker run --rm --add-host=host.docker.internal:host-gateway williamyeh/wrk -t$THREADS -c$CONNS -d$DUR --latency -H "Connection: close" http://host.docker.internal:8080/ > "$OUTDIR/wrk_${TS}.txt" 2>&1
# Stop the stack without deleting volumes or persistent data.
# (An earlier version used `down -v`, which destroyed Redis/Prometheus/Grafana
# volumes; that is intentionally NOT done here.)
docker compose -f "$COMPOSE_FILE" down >/dev/null 2>&1
echo "$OUTDIR/wrk_${TS}.txt"
