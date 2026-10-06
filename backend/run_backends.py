import argparse
import os
import subprocess
import sys
from pathlib import Path

from dotenv import load_dotenv


PROJECT_ROOT = Path(__file__).resolve().parent.parent
BACKEND_SCRIPT = Path(__file__).resolve().parent / "backend.py"

load_dotenv(PROJECT_ROOT / ".env")


def parse_backends(value):
    if not value:
        raise ValueError("BACKENDS is not configured")

    backends = []

    for entry in value.split(","):
        entry = entry.strip()

        if not entry:
            raise ValueError("BACKENDS contains an empty entry")

        if ":" not in entry:
            raise ValueError(
                f"Invalid backend entry: {entry}"
            )

        host, port = entry.rsplit(":", 1)

        if not host or not port:
            raise ValueError(
                f"Invalid backend entry: {entry}"
            )

        try:
            port = int(port)
        except ValueError:
            raise ValueError(
                f"Invalid backend port: {port}"
            )

        if not 1 <= port <= 65535:
            raise ValueError(
                f"Backend port out of range: {port}"
            )

        backends.append((host, port))

    return backends


def main():
    parser = argparse.ArgumentParser(
        description="Start configured backend servers"
    )

    parser.add_argument(
        "--start-port",
        type=int,
        help="Override the first backend port"
    )

    args = parser.parse_args()

    backends = parse_backends(
        os.getenv("BACKENDS")
    )

    if args.start_port is not None:
        backends = [
            (host, args.start_port + index)
            for index, (host, _) in enumerate(backends)
        ]

    processes = []

    try:
        for index, (host, port) in enumerate(backends, start=1):
            name = f"server-{index}"

            print(
                f"Starting {name} "
                f"on {host}:{port}..."
            )

            process = subprocess.Popen(
                [
                    sys.executable,
                    str(BACKEND_SCRIPT),
                    "--port",
                    str(port),
                    "--name",
                    name
                ]
            )

            processes.append(process)

        print(
            f"\nStarted {len(processes)} backend(s)."
        )

        print("Press Ctrl+C to stop all backends.")

        for process in processes:
            process.wait()

    except KeyboardInterrupt:
        print("\nStopping all backends...")

        for process in processes:
            if process.poll() is None:
                process.terminate()

        for process in processes:
            process.wait()

        print("All backends stopped.")


if __name__ == "__main__":
    main()