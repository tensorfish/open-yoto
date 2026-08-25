#!/usr/bin/env python3
"""Serve the admin UI with a stubbed player API for host-side UI testing.

The real UI is served from flash by `components/admin`; this reproduces just
enough of its HTTP contract (login, media listing, mappings, last card) to drive
the page in a browser without hardware. It is a test aid, not part of the
firmware.

    python3 firmware/test/host/serve_admin_ui.py [--port 8099]
"""
from __future__ import annotations

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
INDEX = ROOT / "firmware/components/admin/html/index.html"
CODE = "ABC123"

# One directory tree under the media root, mirroring /api/fs/list output.
TREE = {
    "/sdcard/media": [
        {"name": "story", "path": "/sdcard/media/story", "type": "directory", "size": 0},
        {"name": "cover.img", "path": "/sdcard/media/cover.img", "type": "file", "size": 520},
    ],
    "/sdcard/media/story": [
        {"name": "01-intro.mp3", "path": "/sdcard/media/story/01-intro.mp3", "type": "file", "size": 812345},
        {"name": "02-tale.mp3", "path": "/sdcard/media/story/02-tale.mp3", "type": "file", "size": 923456},
        {"name": "01-intro.img", "path": "/sdcard/media/story/01-intro.img", "type": "file", "size": 520},
    ],
}

MAPPINGS = [
    {
        "url": "https://example.com/card",
        "tracks": ["media/story/01-intro.mp3"],
        "track_images": ["media/story/01-intro.img"],
        "image": "media/cover.img",
    }
]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep the test output quiet
        pass

    def _send(self, code, body=b"", ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, payload, code=200):
        self._send(code, json.dumps(payload).encode(), "application/json")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        query = self.path.split("?", 1)[1] if "?" in self.path else ""

        if path in ("/", "/index.html"):
            self._send(200, INDEX.read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/fs/list":
            target = "/sdcard/media"
            for part in query.split("&"):
                if part.startswith("path="):
                    from urllib.parse import unquote

                    target = unquote(part[5:])
            self._json(TREE.get(target, []))
        elif path == "/api/list":
            self._json(MAPPINGS)
        elif path == "/api/last-card":
            self._json({"captured": True, "uid": "04A1B2C3", "url": "", "seq": 1})
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""

        if path == "/api/login":
            try:
                # The firmware reads JSON {"pin":"..."} or form pin=... (admin.c:1120).
                code = json.loads(body or b"{}").get("pin", "")
            except json.JSONDecodeError:
                code = ""
            if code.upper() == CODE:
                payload = b'{"ok":true}'
                self.send_response(200)
                self.send_header("Set-Cookie", "oy=stub; Path=/; HttpOnly")
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            else:
                self._json({"error": "bad pin"}, 403)
        elif path == "/api/add":
            try:
                MAPPINGS.append(json.loads(body or b"{}"))
            except json.JSONDecodeError:
                pass
            self._json({"ok": True})
        elif path in ("/api/delete", "/api/control/play", "/api/control/stop",
                      "/api/control/display", "/api/control/clear",
                      "/api/fs/upload", "/api/fs/mkdir", "/api/fs/create",
                      "/api/fs/rename", "/api/fs/delete"):
            self._json({"ok": True})
        elif path == "/api/card/write":
            self._json({"ok": True, "seq": 2})
        else:
            self._json({"error": "not found"}, 404)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8099)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"admin UI stub on http://127.0.0.1:{args.port}/ (code {CODE})", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
