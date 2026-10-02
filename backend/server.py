from http.server import BaseHTTPRequestHandler, HTTPServer
import json


class Handler(BaseHTTPRequestHandler):

    def do_GET(self):

        response = {
            "backend": "server-1",
            "message": "Hello from backend"
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


server = HTTPServer(
    ("localhost", 9001),
    Handler
)

print("Backend server listening on port 9001...")

server.serve_forever()