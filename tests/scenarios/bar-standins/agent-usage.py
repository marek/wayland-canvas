#!/usr/bin/env python3
"""The Anthropic usage-endpoint stand-in for the bar scenario.

Serves the usage payload the scenario stages under the run's scratch, on
loopback only, so the agents panel's Claude probe is answered without a
network. Every request is logged — path, Authorization, the oauth beta
header — so the run can prove what was asked for, that the panel's own
token was the one presented, and that no real endpoint was ever reached.
"""

import http.server
import json
import os
import sys

ROOT = os.environ.get("CANVAS_BAR_DIR") or os.path.join(
    os.environ.get("CANVAS_TMP", "tmp"), "scenarios", "bar")
LOG = os.path.join(ROOT, "usage-requests.log")
PAYLOAD = os.path.join(ROOT, "usage-response.json")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        entry = {
            "path": self.path,
            "authorization": self.headers.get("Authorization", ""),
            "beta": self.headers.get("anthropic-beta", ""),
        }
        with open(LOG, "a") as log:
            log.write(json.dumps(entry) + "\n")
        try:
            with open(PAYLOAD, "rb") as body:
                data = body.read()
        except OSError:
            data = b"{}"
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        pass  # the log above is the record that matters


def main():
    # Loopback only, a kernel-chosen port: the scenario reads it off the
    # READY line and points the collector at it.
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    print(f"READY {server.server_address[1]}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
