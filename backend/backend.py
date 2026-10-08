from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
import argparse
import json
import time


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        if self.server.delay_ms > 0:
            time.sleep(self.server.delay_ms / 1000)

        if self.path == "/health":
            response = {
                "status": "ok",
                "backend": self.server.backend_name
            }
        else:
            response = {
                "backend": self.server.backend_name,
                "port": self.server.server_port
            }

        body = json.dumps(response).encode()

        self.send_response(200)
        self.send_header(
            "Content-Type",
            "application/json"
        )
        self.send_header(
            "Content-Length",
            str(len(body))
        )
        self.end_headers()

        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


def main():
    parser = argparse.ArgumentParser(
        description="Run a backend server"
    )

    parser.add_argument(
        "--port",
        type=int,
        required=True,
        help="Port on which the backend listens"
    )

    parser.add_argument(
        "--name",
        type=str,
        default=None,
        help="Backend name"
    )

    parser.add_argument(
        "--delay-ms",
        type=int,
        default=0,
        help="Artificial response delay in milliseconds"
    )

    args = parser.parse_args()

    if args.delay_ms < 0:
        raise ValueError(
            "delay-ms must be non-negative"
        )

    backend_name = (
        args.name
        or f"server-{args.port}"
    )

    server = ThreadedHTTPServer(
        ("localhost", args.port),
        Handler
    )

    server.backend_name = backend_name
    server.delay_ms = args.delay_ms

    print(
        f"Backend {backend_name} listening "
        f"on port {args.port} "
        f"(delay={args.delay_ms}ms)..."
    )

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print(
            f"\nStopping {backend_name}..."
        )
    finally:
        server.server_close()


if __name__ == "__main__":
    main()