from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
import json


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):

        if self.path == "/health":
            response = {
                "status": "ok",
                "backend": "server-3"
            }

        else:
            response = {
                "backend": "server-3",
                "port": 9003
            }

        body = json.dumps(response).encode()

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()

        self.wfile.write(body)


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


server = ThreadedHTTPServer(("localhost", 9003), Handler)

print("Backend server 1 listening on port 9003...")

server.serve_forever()