#!/usr/bin/env python3
"""Preview README.md as GitHub shows it, at http://127.0.0.1:<port>/.

Every Markdown file of the checkout is rendered on request through GitHub's
own Markdown API, the renderer github.com uses for a repository's files, and
shown in GitHub's stylesheet at the width of a repository's README; the
pictures, the video and every other file are served from the checkout, so
relative links resolve as they will on GitHub. Reload the page after an
edit. The API takes 60 requests an hour without a login; with the GitHub
CLI signed in, its token raises that to 5000.

usage: tools/readme-preview.py [--port N] [--no-open]
"""

import argparse
import html
import http.server
import json
import mimetypes
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STYLESHEET = "https://cdnjs.cloudflare.com/ajax/libs/github-markdown-css/5.8.1/github-markdown.min.css"
PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>{title} — preview</title>
<meta name="color-scheme" content="light dark">
<link rel="stylesheet" href="{stylesheet}">
<style>
body {{ margin: 0; background: Canvas; }}
.markdown-body {{ box-sizing: border-box; max-width: 1012px; margin: 0 auto; padding: 32px 45px; }}
</style></head>
<body><article class="markdown-body">{body}</article></body></html>
"""


def github_token():
    try:
        out = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.stdout.strip() or None


def render(markdown, token):
    request = urllib.request.Request(
        "https://api.github.com/markdown",
        data=json.dumps({"text": markdown, "mode": "markdown"}).encode(),
        headers={"Accept": "application/vnd.github+json", "User-Agent": "wayland-canvas readme-preview"})
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read().decode()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
        target = (ROOT / path.lstrip("/")).resolve()
        if path == "/":
            target = ROOT / "README.md"
        elif target.is_dir():
            target = target / "README.md"
        if not target.is_file() or ROOT not in target.parents:
            self.answer(404, "text/plain", b"not in the checkout\n")
            return
        if target.suffix != ".md":
            kind = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
            self.answer(200, kind, target.read_bytes())
            return
        try:
            body = render(target.read_text(encoding="utf-8"), self.server.token)
        except urllib.error.URLError as error:
            self.answer(502, "text/plain", f"GitHub's Markdown API: {error}\n".encode())
            return
        page = PAGE.format(title=html.escape(target.name), stylesheet=STYLESHEET, body=body)
        self.answer(200, "text/html; charset=utf-8", page.encode())

    def answer(self, status, kind, body):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def main():
    parser = argparse.ArgumentParser(description="Preview README.md as GitHub shows it.")
    parser.add_argument("--port", type=int, default=0, help="the port to serve on (a free one by default)")
    parser.add_argument("--no-open", action="store_true", help="print the address, open no browser")
    args = parser.parse_args()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.token = github_token()
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    print(f"readme-preview: {url} (Ctrl+C stops it)", flush=True)
    if not args.no_open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    server.server_close()


if __name__ == "__main__":
    sys.exit(main())
