"""Web pages for the tour, captured once and replayed after.

A page is captured with headless Chrome at the width of the window that
shows it and at scale 2: a picture of the whole rendered page, its title,
its background and its icon. On every later run a local HTTPS server
answers the tour's Chrome at the page's own URL with a page holding that
picture, so the pictures keep the day the page was captured however the
site changes, and nothing the tour shows is fetched live. Chrome reaches
the server through --host-resolver-rules and accepts its certificate by
--ignore-certificate-errors-spki-list, as Chromium's own Web Page Replay
does it.
"""

import base64
import hashlib
import html
import http.server
import json
import os
import ssl
import subprocess
import threading
import time
import urllib.parse
import urllib.request

# The user agent of the Chrome the tour runs: a site sees a desktop
# browser, not a headless one.
USER_AGENT = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36")
# How much of a page is kept, in logical px: more than any window shows.
CAPTURE_HEIGHT = 2400
# Where a replayed page finds its picture and icon on its own host.
PICTURE_PATH = "/.media-replay/page.png"
ICON_PATH = "/.media-replay/icon"


class Cdp:
    """Chrome DevTools over --remote-debugging-pipe: one JSON message per
    NUL-terminated chunk, commands on fd 3 and replies on fd 4."""

    def __init__(self, argv):
        to_chrome, self._commands = os.pipe()
        self._replies, from_chrome = os.pipe()

        def wire():
            os.dup2(to_chrome, 3)
            os.dup2(from_chrome, 4)

        self.proc = subprocess.Popen(argv, preexec_fn=wire, pass_fds=(3, 4),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.close(to_chrome)
        os.close(from_chrome)
        self._next = 0
        self._buffer = b""
        self.events = []

    def _read(self):
        while b"\0" not in self._buffer:
            chunk = os.read(self._replies, 1 << 20)
            if not chunk:
                raise RuntimeError("Chrome closed the DevTools pipe")
            self._buffer += chunk
        message, self._buffer = self._buffer.split(b"\0", 1)
        return json.loads(message)

    def call(self, method, params=None, session=None):
        self._next += 1
        message = {"id": self._next, "method": method, "params": params or {}}
        if session is not None:
            message["sessionId"] = session
        os.write(self._commands, json.dumps(message).encode() + b"\0")
        while True:
            reply = self._read()
            if reply.get("id") == self._next:
                if "error" in reply:
                    raise RuntimeError(f"{method}: {reply['error'].get('message')}")
                return reply.get("result", {})
            self.events.append(reply)

    def wait_for(self, method, seconds):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if any(e.get("method") == method for e in self.events):
                return True
            # a command that answers at once lets the events queued before it in
            self.call("Browser.getVersion")
            time.sleep(0.1)
        return False

    def evaluate(self, expression, session):
        result = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True},
                           session=session)
        return result.get("result", {}).get("value")

    def close(self):
        try:
            self.call("Browser.close")
        except (RuntimeError, OSError):
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


def _fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=20) as response:
        return response.read(), response.headers.get_content_type()


def record(url, width, base, profile, remove=""):
    """Captures `url` at `width` logical px and scale 2 as `base`.png, with
    its title, background and URL in `base`.json and its icon beside them.
    `remove` is a CSS selector list of what to take out first: the ads."""
    cdp = Cdp(["google-chrome-stable", "--headless=new", "--remote-debugging-pipe",
               f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
               "--hide-scrollbars", f"--user-agent={USER_AGENT}", "about:blank"])
    try:
        page = next(t for t in cdp.call("Target.getTargets")["targetInfos"] if t["type"] == "page")
        session = cdp.call("Target.attachToTarget",
                           {"targetId": page["targetId"], "flatten": True})["sessionId"]
        cdp.call("Page.enable", session=session)
        cdp.call("Emulation.setDeviceMetricsOverride",
                 {"width": width, "height": CAPTURE_HEIGHT, "deviceScaleFactor": 2,
                  "mobile": False}, session=session)
        cdp.events.clear()
        cdp.call("Page.navigate", {"url": url}, session=session)
        if not cdp.wait_for("Page.loadEventFired", 45):
            raise RuntimeError(f"{url} did not finish loading")
        time.sleep(6)  # what the page fetches after its load event
        if remove:
            cdp.evaluate(f"document.querySelectorAll({json.dumps(remove)}).forEach(e => e.remove())",
                         session)
            time.sleep(1)
        # A fragment's anchor (an id, or a DocBook <a name>) starts the
        # capture, as a browser opening the URL scrolls to it.
        meta = cdp.evaluate(
            "(() => { const f = decodeURIComponent(location.hash.slice(1));"
            " const a = f && (document.getElementById(f) || document.getElementsByName(f)[0]);"
            " return {url: location.href, title: document.title,"
            " background: getComputedStyle(document.body).backgroundColor,"
            " icon: (document.querySelector('link[rel~=\"icon\"]') || {}).href || '',"
            " top: a ? Math.max(0, a.getBoundingClientRect().top + scrollY - 8) : 0}; })()",
            session)
        shot = cdp.call("Page.captureScreenshot",
                        {"format": "png", "captureBeyondViewport": True,
                         "clip": {"x": 0, "y": meta.pop("top"), "width": width,
                                  "height": CAPTURE_HEIGHT, "scale": 1}},
                        session=session)
    finally:
        cdp.close()
    with open(base + ".png", "wb") as f:
        f.write(base64.b64decode(shot["data"]))
    icon = meta.pop("icon")
    meta["icon_type"] = ""
    if icon:
        try:
            body, kind = _fetch(icon)
            with open(base + ".icon", "wb") as f:
                f.write(body)
            meta["icon_type"] = kind
        except OSError:
            pass
    with open(base + ".json", "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=1)


class Snapshot:
    """One captured page, read back from `base`.json and its picture."""

    def __init__(self, base):
        with open(base + ".json", encoding="utf-8") as f:
            meta = json.load(f)
        self.url = meta["url"]
        parts = urllib.parse.urlsplit(self.url)
        self.host = parts.netloc.lower()
        self.path = parts.path or "/"
        self.query = parts.query
        with open(base + ".png", "rb") as f:
            self.picture = f.read()
        self.icon, self.icon_type = b"", meta.get("icon_type", "")
        if self.icon_type:
            with open(base + ".icon", "rb") as f:
                self.icon = f.read()
        icon_link = f'<link rel="icon" href="{ICON_PATH}">' if self.icon else ""
        self.page = (
            "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
            f"<title>{html.escape(meta['title'])}</title>{icon_link}"
            f"<style>html{{scrollbar-width:none;background:{meta['background']}}}"
            "body{margin:0}img{display:block;width:100%}</style></head>"
            f"<body><img src=\"{PICTURE_PATH}\" alt=\"\"></body></html>\n").encode()


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        host = self.headers.get("Host", "").split(":")[0].lower()
        path, _, query = self.path.partition("?")
        answer = None
        for snapshot in self.server.snapshots:
            if snapshot.host != host:
                continue
            if path == PICTURE_PATH:
                answer = ("image/png", snapshot.picture)
            elif path == ICON_PATH and snapshot.icon:
                answer = (snapshot.icon_type, snapshot.icon)
            elif path == snapshot.path and query == snapshot.query:
                answer = ("text/html; charset=utf-8", snapshot.page)
        if answer is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        kind, body = answer
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the browser went away mid-answer: nothing to finish

    def log_message(self, format, *args):
        pass


class ReplayServer:
    """Serves the captured pages over HTTPS on a free local port, from a
    thread, with a certificate of its own made in `workdir`."""

    def __init__(self, bases, workdir):
        key, cert = os.path.join(workdir, "replay-key.pem"), os.path.join(workdir, "replay-cert.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                        "ec_paramgen_curve:prime256v1", "-nodes", "-days", "30",
                        "-subj", "/CN=wayland-canvas media replay", "-keyout", key, "-out", cert],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pem = subprocess.run(["openssl", "x509", "-in", cert, "-pubkey", "-noout"],
                             check=True, capture_output=True).stdout
        spki = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "der"], input=pem,
                              check=True, capture_output=True).stdout
        self.spki = base64.b64encode(hashlib.sha256(spki).digest()).decode()
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.httpd.daemon_threads = True
        self.httpd.snapshots = [Snapshot(base) for base in bases]
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.httpd.socket = context.wrap_socket(self.httpd.socket, server_side=True)
        self.port = self.httpd.server_address[1]
        self._thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self._thread.start()

    def chrome_flags(self):
        """The flags, one a line: every host resolves to the server, so the
        tour's Chrome reaches nothing but the captured pages; --test-type
        keeps Chrome's warning about such flags out of the pictures."""
        return "\n".join([f"--host-resolver-rules=MAP * 127.0.0.1:{self.port}",
                          f"--ignore-certificate-errors-spki-list={self.spki}",
                          "--disable-quic", "--test-type"])

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
