# syntax=docker/dockerfile:1

# Sample backend used only to demonstrate proxying, load balancing and
# failover. It reuses the existing backend/backend.py implementation.
FROM python:3.12-slim

RUN useradd --system --create-home --uid 10002 backend

WORKDIR /app

COPY backend.py /app/backend.py

USER backend

# Arguments (--host/--port/--name) are supplied by the Compose service.
ENTRYPOINT ["python", "/app/backend.py"]
