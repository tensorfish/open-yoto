#!/usr/bin/env python3
"""Serve the admin UI with a stubbed player API for host-side UI testing.

The real UI is served from flash by `components/admin`; this reproduces enough of
its HTTP contract to drive the page in a browser without hardware, including the
pieces the card-organisation work needs: nested file downloads for cover
previews, two first-level media directories (one already saved as a card, one
not), directory creation and uploads.

It is a test aid, not part of the firmware. Contract details are mirrored from
firmware/components/admin/admin.c.

    python3 firmware/test/host/serve_admin_ui.py [--port 8099]
"""
from __future__ import annotations

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

ROOT = Path(__file__).resolve().parents[3]
INDEX = ROOT / "firmware/components/admin/html/index.html"
CODE = "ABC123"


def oyim(colour: int) -> bytes:
    """One 520-byte OYIM v1 16x16 RGB565 frame, as the player stores .img files.

    Header: 'OYIM', version 1, RGB565 flag 1, width, height, then 512 bytes of
    little-endian RGB565 (app_main.c render_image / PLAYER_COLOR_IMAGE_*).
    """
    return b"OYIM" + bytes((1, 1, 16, 16)) + colour.to_bytes(2, "little") * 256


# Distinct colours so a thumbnail is visibly correct, not merely present.
IMAGES = {
    "/sdcard/media/cover.img": oyim(0xF800),           # red
    "/sdcard/media/story/01-intro.img": oyim(0x07E0),  # green
    "/sdcard/media/story/02-tale.img": oyim(0x001F),   # blue
    "/sdcard/media/rhymes/01-song.img": oyim(0xFFE0),  # yellow
}

# Two first-level directories under the media root: 'story' is already saved as a
# card, 'rhymes' is not, so "already saved" has both states to render.
TREE = {
    "/sdcard/media": [
        {"name": "story", "path": "/sdcard/media/story", "type": "directory", "size": 0},
        {"name": "rhymes", "path": "/sdcard/media/rhymes", "type": "directory", "size": 0},
        {"name": "cover.img", "path": "/sdcard/media/cover.img", "type": "file", "size": 520},
    ],
    "/sdcard/media/story": [
        {"name": "01-intro.mp3", "path": "/sdcard/media/story/01-intro.mp3", "type": "file", "size": 812345},
        {"name": "02-tale.mp3", "path": "/sdcard/media/story/02-tale.mp3", "type": "file", "size": 923456},
        {"name": "01-intro.img", "path": "/sdcard/media/story/01-intro.img", "type": "file", "size": 520},
        {"name": "02-tale.img", "path": "/sdcard/media/story/02-tale.img", "type": "file", "size": 520},
    ],
    "/sdcard/media/rhymes": [
        {"name": "01-song.mp3", "path": "/sdcard/media/rhymes/01-song.mp3", "type": "file", "size": 512000},
        {"name": "01-song.img", "path": "/sdcard/media/rhymes/01-song.img", "type": "file", "size": 520},
    ],
}

MAPPINGS = [
    {
        "url": "https://example.com/story",
        "tracks": ["media/story/01-intro.mp3", "media/story/02-tale.mp3"],
        "track_images": ["media/story/01-intro.img", "media/story/02-tale.img"],
        "image": "media/cover.img",
    }
]

OK_POSTS = {
    "/api/control/play", "/api/control/stop", "/api/control/display",
    "/api/control/clear", "/api/fs/rename", "/api/fs/create",
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep test output quiet
        pass

    def _send(self, code, body=b"", ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, payload, code=200):
        self._send(code, json.dumps(payload).encode())

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        if path in ("/", "/index.html"):
            self._send(200, INDEX.read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/fs/list":
            target = (query.get("path") or ["/sdcard/media"])[0]
            self._json(TREE.get(target, []))
        elif path == "/api/fs/file":
            target = (query.get("path") or [""])[0]
            if target in IMAGES:
                self._send(200, IMAGES[target], "application/octet-stream")
            elif any(target == e["path"] for items in TREE.values() for e in items):
                self._send(200, b"\0" * 64, "application/octet-stream")
            else:
                self._json({"error": "not found"}, 404)
        elif path.startswith("/media/"):
            # The firmware rejects '/' after /media/, so only flat names resolve.
            name = unquote(path[len("/media/"):])
            if "/" in name:
                self._json({"error": "nested path rejected"}, 400)
            else:
                target = "/sdcard/media/" + name
                if target in IMAGES:
                    self._send(200, IMAGES[target], "application/octet-stream")
                else:
                    self._json({"error": "not found"}, 404)
        elif path == "/api/list":
            self._json(MAPPINGS)
        elif path == "/api/last-card":
            self._json({"captured": True, "uid": "04A1B2C3", "url": "", "seq": 1})
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""

        if path == "/api/login":
            try:
                # The firmware reads JSON {"pin":"..."} or form pin=... (admin.c:1113).
                pin = json.loads(body or b"{}").get("pin", "")
            except json.JSONDecodeError:
                pin = ""
            if pin.upper() == CODE:
                payload = b'{"ok":true}'
                self.send_response(200)
                self.send_header("Set-Cookie",
                                 "yoto_session=stub; Path=/; HttpOnly; SameSite=Strict")
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            else:
                self._json({"error": "bad pin"}, 403)
        elif path == "/api/add":
            try:
                card = json.loads(body or b"{}")
            except json.JSONDecodeError:
                self._json({"error": "bad json"}, 400)
                return
            if len(card.get("tracks", [])) > 32:      # ADMIN_MAX_TRACKS
                self._json({"error": "too many tracks"}, 400)
                return
            MAPPINGS[:] = [m for m in MAPPINGS if m.get("url") != card.get("url")]
            MAPPINGS.append(card)
            self._json({"ok": True})
        elif path == "/api/delete":
            try:
                url = json.loads(body or b"{}").get("url", "")
            except json.JSONDecodeError:
                url = ""
            MAPPINGS[:] = [m for m in MAPPINGS if m.get("url") != url]
            self._json({"ok": True})
        elif path == "/api/card/write":
            self._json({"ok": True, "seq": 2})
        elif path == "/api/fs/mkdir":
            try:
                target = json.loads(body or b"{}").get("path", "")
            except json.JSONDecodeError:
                target = ""
            if not target.startswith("/sdcard"):
                self._json({"error": "bad path"}, 400)
                return
            if target in TREE:                        # already exists -> ok (EEXIST)
                self._json({"ok": True})
                return
            parent = target.rsplit("/", 1)[0]
            if parent not in TREE:                    # mkdir is single level only
                self._json({"error": "parent missing"}, 400)
                return
            TREE.setdefault(target, [])
            name = target.rsplit("/", 1)[1]
            if not any(e["path"] == target for e in TREE[parent]):
                TREE[parent].append({"name": name, "path": target,
                                     "type": "directory", "size": 0})
            self._json({"ok": True})
        elif path == "/api/fs/upload":
            target = (query.get("path") or [""])[0]
            if len(body) > 4 * 1024 * 1024:           # ADMIN_BODY_MAX
                self._json({"error": "too large"}, 400)
                return
            parent = target.rsplit("/", 1)[0]
            if parent not in TREE:
                self._json({"error": "parent missing"}, 400)
                return
            name = target.rsplit("/", 1)[1]
            TREE[parent] = [e for e in TREE[parent] if e["path"] != target]
            TREE[parent].append({"name": name, "path": target, "type": "file",
                                 "size": len(body)})
            if target.endswith(".img"):
                IMAGES[target] = body
            self._json({"ok": True})
        elif path == "/api/fs/delete":
            try:
                target = json.loads(body or b"{}").get("path", "")
            except json.JSONDecodeError:
                target = ""
            for items in TREE.values():
                items[:] = [e for e in items if e["path"] != target]
            IMAGES.pop(target, None)
            self._json({"ok": True})
        elif path in OK_POSTS:
            self._json({"ok": True})
        else:
            self._json({"error": "not found"}, 404)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8099)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"admin UI stub on http://127.0.0.1:{args.port}/ (pin {CODE})", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
