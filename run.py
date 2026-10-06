import os
import signal
import subprocess
import sys
from pathlib import Path

from dotenv import load_dotenv


PROJECT_ROOT = Path(__file__).resolve().parent

load_dotenv(PROJECT_ROOT / ".env")


backend_process = None
gateway_process = None


def shutdown():
    global backend_process
    global gateway_process

    print("\nShutting down...")

    if gateway_process is not None:
        if gateway_process.poll() is None:
            gateway_process.terminate()

    if backend_process is not None:
        if backend_process.poll() is None:
            backend_process.terminate()

    if gateway_process is not None:
        gateway_process.wait()

    if backend_process is not None:
        backend_process.wait()

    print("All services stopped.")


def handle_signal(signum, frame):
    shutdown()
    sys.exit(0)


def main():
    global backend_process
    global gateway_process

    signal.signal(
        signal.SIGINT,
        handle_signal
    )

    signal.signal(
        signal.SIGTERM,
        handle_signal
    )

    required_variables = [
        "GATEWAY_PORT",
        "BACKENDS",
        "RATE_LIMITING_ENABLED",
        "RATE_LIMIT_CAPACITY",
        "RATE_LIMIT_REFILL_RATE",
        "REDIS_HOST",
        "REDIS_PORT",
    ]

    missing = [
        name
        for name in required_variables
        if not os.getenv(name)
    ]

    if missing:
        raise RuntimeError(
            "Missing configuration: "
            + ", ".join(missing)
        )

    print("Starting backend servers...")

    backend_process = subprocess.Popen(
        [
            sys.executable,
            str(
                PROJECT_ROOT
                / "backend"
                / "run_backends.py"
            )
        ],
        cwd=PROJECT_ROOT,
    )

    print("Starting API gateway...")

    gateway_executable = (
        PROJECT_ROOT
        / "build"
        / "api_gateway.exe"
    )

    if not gateway_executable.exists():
        raise RuntimeError(
            f"Gateway executable not found: "
            f"{gateway_executable}"
        )

    gateway_process = subprocess.Popen(
        [str(gateway_executable)],
        cwd=PROJECT_ROOT,
        env=os.environ.copy(),
    )

    print("\nAPI Gateway is running.")
    print("Press Ctrl+C to stop everything.")

    try:
        gateway_process.wait()
    finally:
        shutdown()


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"Error: {exc}")
        shutdown()
        sys.exit(1)