#!/usr/bin/env python3
"""Generate a Docker Compose file with a configurable number of sample backends.

The gateway registers backends from a static, comma-separated ``BACKENDS`` list
(``host:port`` entries) and health-checks each one independently. Docker Compose
cannot loop over a service definition, and scaling a single service does not
create separately addressable backends, so this generator produces a Compose
file with ``N`` distinct backend services (``backend-1`` ... ``backend-N``),
each with its own service name, port, health check and gateway startup
dependency.

The generator reads the hand-written ``docker-compose.yml`` as the single source
of truth and reuses its Redis, gateway, Prometheus and Grafana service
definitions unchanged. Only the backend services and the gateway's ``BACKENDS``
list / ``depends_on`` block are regenerated. This keeps the generated file in
sync with the base stack and avoids duplicating (and drifting from) the Redis,
Prometheus and Grafana configuration.

Usage (Windows / native workflow)::

    python scripts/generate_compose.py --count 4
    docker compose -f docker-compose.generated.yml up --build
    docker compose -f docker-compose.generated.yml down

The existing ``docker-compose.yml`` (two backends) is left untouched and keeps
working on its own.

Practical limits
----------------
* Ports: each backend gets ``base_port + index``; the requested count must fit
  in the TCP port range, i.e. ``base_port + count - 1 <= 65535``.
* Resources: every backend is a separate container running a small CPython HTTP
  server. Real limits are host CPU/RAM and Docker bridge address space, not the
  port range. Counts in the low tens are comfortable on a developer laptop;
  larger counts should be sized against available memory.
"""

import argparse
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover - environment guidance
    sys.stderr.write(
        "Error: PyYAML is required to generate the Compose file.\n"
        "Install it with: python -m pip install pyyaml\n"
    )
    sys.exit(2)


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BASE_FILE = REPO_ROOT / "docker-compose.yml"
DEFAULT_OUTPUT = REPO_ROOT / "docker-compose.generated.yml"
DEFAULT_BASE_PORT = 9001
DEFAULT_COUNT = 2
DEFAULT_IMAGE = "cpp-api-gateway-backend:local"
NETWORK_NAME = "gateway"
BACKEND_PREFIX = "backend-"

# Matches services this generator owns (both the generated "backend-1" style
# and the hand-written "backend-a"/"backend-b" base services).
BACKEND_SERVICE_RE = re.compile(r"^backend-[0-9a-zA-Z]+$")


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description=(
            "Generate a docker-compose file with N uniquely addressable "
            "sample backend instances and a matching gateway BACKENDS list."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python scripts/generate_compose.py --count 2\n"
            "  python scripts/generate_compose.py --count 8 --base-port 9501\n"
            "  python scripts/generate_compose.py --count 4 --output -\n"
        ),
    )

    parser.add_argument(
        "--count",
        type=int,
        default=DEFAULT_COUNT,
        help=(
            "Number of backend instances to generate (default: %(default)s). "
            "Must be >= 1."
        ),
    )

    parser.add_argument(
        "--base-port",
        type=int,
        default=DEFAULT_BASE_PORT,
        help=(
            "Port assigned to backend-1; later backends increment from it "
            "(default: %(default)s)."
        ),
    )

    parser.add_argument(
        "--base-file",
        type=Path,
        default=DEFAULT_BASE_FILE,
        help=(
            "Compose file used as the template for non-backend services "
            "(default: %(default)s)."
        ),
    )

    parser.add_argument(
        "--output",
        type=str,
        default=str(DEFAULT_OUTPUT),
        help=(
            "Where to write the generated Compose file, or '-' for stdout "
            "(default: %(default)s)."
        ),
    )

    parser.add_argument(
        "--image",
        type=str,
        default=DEFAULT_IMAGE,
        help="Image name/tag for the generated backend services "
        "(default: %(default)s).",
    )

    return parser.parse_args(argv)


def validate(count, base_port):
    if count < 1:
        raise ValueError(
            f"--count must be >= 1 (got {count}). "
            "At least one backend is required."
        )

    if not 1 <= base_port <= 65535:
        raise ValueError(
            f"--base-port must be between 1 and 65535 (got {base_port})."
        )

    if base_port + count - 1 > 65535:
        raise ValueError(
            f"--count {count} with --base-port {base_port} exceeds the TCP "
            f"port range (highest port would be {base_port + count - 1}). "
            "Lower the count or the base port."
        )


def load_base(path):
    if not path.is_file():
        raise ValueError(f"Base Compose file not found: {path}")

    with path.open("r", encoding="utf-8") as handle:
        data = yaml.safe_load(handle)

    if not isinstance(data, dict) or "services" not in data:
        raise ValueError(
            f"{path} is not a valid Compose file (missing 'services')."
        )

    if "gateway" not in data["services"]:
        raise ValueError(f"{path} does not define a 'gateway' service.")

    return data


def backend_service(index, port, image):
    name = f"{BACKEND_PREFIX}{index}"

    health_url = f"http://127.0.0.1:{port}/health"

    healthcheck_test = (
        "python -c \"import urllib.request; "
        f"urllib.request.urlopen('{health_url}', timeout=2)\""
    )

    return {
        "build": {"context": "./backend"},
        "image": image,
        "container_name": f"cpp-api-gateway-{name}",
        "command": [
            "--host",
            "0.0.0.0",
            "--port",
            str(port),
            "--name",
            name,
        ],
        "healthcheck": {
            "test": ["CMD-SHELL", healthcheck_test],
            "interval": "5s",
            "timeout": "3s",
            "retries": 12,
            "start_period": "5s",
        },
        "networks": [NETWORK_NAME],
    }


def build_compose(base, count, base_port, image):
    services = base["services"]

    for name in list(services):
        if BACKEND_SERVICE_RE.match(name):
            del services[name]

    gateway = services["gateway"]

    depends_on = gateway.setdefault("depends_on", {})
    for name in list(depends_on):
        if BACKEND_SERVICE_RE.match(name):
            del depends_on[name]

    environment = gateway.setdefault("environment", {})

    backend_names = []
    new_backends = {}

    for index in range(1, count + 1):
        name = f"{BACKEND_PREFIX}{index}"
        port = base_port + index - 1

        new_backends[name] = backend_service(index, port, image)
        backend_names.append(name)

        depends_on[name] = {"condition": "service_healthy"}

    environment["BACKENDS"] = ",".join(
        f"{name}:{base_port + i}"
        for i, name in enumerate(backend_names)
    )

    # Keep backends grouped together and placed before the gateway for
    # readability, while leaving every other service untouched.
    reordered = {}
    for name, service in services.items():
        if name == "gateway":
            reordered.update(new_backends)
        reordered[name] = service

    if "gateway" not in reordered:  # defensive; gateway is required above
        reordered.update(new_backends)

    base["services"] = reordered

    return base, backend_names


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])

    try:
        validate(args.count, args.base_port)
        base = load_base(args.base_file)
        compose, backend_names = build_compose(
            base,
            args.count,
            args.base_port,
            args.image,
        )
    except ValueError as exc:
        sys.stderr.write(f"Error: {exc}\n")
        return 2

    document = yaml.safe_dump(
        compose,
        sort_keys=False,
        default_flow_style=False,
        width=10000,
    )

    header = (
        "# Generated by scripts/generate_compose.py -- do not edit by hand.\n"
        f"# Backends: {len(backend_names)} "
        f"({backend_names[0]} .. {backend_names[-1]})\n"
        "# Regenerate with: "
        f"python scripts/generate_compose.py --count {args.count} "
        f"--base-port {args.base_port}\n"
        "#\n"
        "# Run:  docker compose -f docker-compose.generated.yml up --build\n"
        "# Stop: docker compose -f docker-compose.generated.yml down\n"
    )

    rendered = header + document

    if args.output == "-":
        sys.stdout.write(rendered)
        return 0

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(rendered)

    sys.stderr.write(
        f"Wrote {len(backend_names)} backend(s) to {output_path}\n"
        f"BACKENDS={compose['services']['gateway']['environment']['BACKENDS']}\n"
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
