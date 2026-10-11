#!/bin/bash
set -e
DUR=${DUR:-15s}
THREADS=${THREADS:-4}
CONNS=${CONNS:-200}
URL=http://host.docker.internal:8080/
OUT=${OUT:-bench.out}
docker run --rm --add-host=host.docker.internal:host-gateway williamyeh/wrk -t$THREADS -c$CONNS -d$DUR --latency -H "Connection: close" $URL 2>&1 | tee "$OUT"
