from http.server import BaseHTTPRequestHandler, HTTPServer
import json


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        json.loads(self.rfile.read(length))
        body = json.dumps({
            "model": "typesafe/jev-1.13-mock",
            "answers": {
                "bias": {
                    "type": "choice",
                    "choice": "long",
                    "probabilities": {"long": 0.9, "short": 0.1},
                    "confidence": 0.8,
                },
                "intent": {
                    "type": "choice",
                    "choice": "hold",
                    "probabilities": {"open": 0.1, "close": 0.1, "hold": 0.8},
                    "confidence": 0.7,
                },
            },
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


HTTPServer(("127.0.0.1", 18765), Handler).serve_forever()
