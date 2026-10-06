from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
import argparse
import json


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
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
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
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

    args = parser.parse_args()

    backend_name = args.name or f"server-{args.port}"

    server = ThreadedHTTPServer(
        ("localhost", args.port),
        Handler
    )

    server.backend_name = backend_name

    print(
        f"Backend {backend_name} listening "
        f"on port {args.port}..."
    )

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print(f"\nStopping {backend_name}...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()