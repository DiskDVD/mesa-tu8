#!/usr/bin/env python3
import http.server
import socketserver
import os

PORT = 5000
DOCS_DIR = os.path.join(os.path.dirname(__file__), "docs-html")

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DOCS_DIR, **kwargs)

    def log_message(self, format, *args):
        print(f"[{self.address_string()}] {format % args}")

with socketserver.TCPServer(("0.0.0.0", PORT), Handler) as httpd:
    print(f"Serving Mesa 3D docs at http://0.0.0.0:{PORT}")
    httpd.serve_forever()
