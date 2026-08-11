#!/usr/bin/env python3

import http.server
import os


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        plugin_marker = self.headers.get("x-worker-init-plugin", "absent")
        print(f"path={self.path} plugin={plugin_marker}", flush=True)
        body = f"upstream=reached plugin={plugin_marker}\n".encode()
        self.send_response(200)
        self.send_header("content-type", "text/plain")
        self.send_header("content-length", str(len(body)))
        self.send_header("x-upstream-marker", "reached")
        self.send_header("x-plugin-marker", plugin_marker)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return


port = int(os.environ.get("UPSTREAM_PORT", "3084"))
http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
