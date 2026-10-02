from http.server import BaseHTTPRequestHandler, HTTPServer
import json


class Handler(BaseHTTPRequestHandler):

    def do_GET(self):

        if self.path == "/health":
            response = {
                "status": "ok",
                "backend": "server-1"
            }

        else:
            response = {
                "backend": "server-1",
                "port": 9001
            }

        body = json.dumps(response).encode()

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()

        self.wfile.write(body)


server = HTTPServer(("localhost", 9001), Handler)

print("Backend server 1 listening on port 9001...")

server.serve_forever()