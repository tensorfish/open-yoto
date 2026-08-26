#!/usr/bin/env python3
"""Serve the SD-hosted admin UI flow with a stubbed player API.

The device embeds only the PIN-guarded uploader. Its root redirects to that
page until an index.html has been stored on the SD card, then serves the stored
page. This host fixture mirrors that installation path and enough of the
custom-page API contract to drive it in a browser.

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
UPLOAD = ROOT / "firmware/components/admin/html/upload.html"
HOSTED_INDEX: bytes | None = None


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

# The most recently scanned card. Card writes update this value so browser
# workflow tests can verify linked-playlist overwrite confirmation.
LAST_CARD = {
    "captured": True,
    "uid": "04A1B2C3",
    "url": "https://example.com/story",
    "seq": 1,
}

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
            if HOSTED_INDEX is None:
                self.send_response(302)
                self.send_header("Location", "/upload")
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                self._send(200, HOSTED_INDEX, "text/html; charset=utf-8")
        elif path == "/upload":
            self._send(200, UPLOAD.read_bytes(), "text/html; charset=utf-8")
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
            card = dict(LAST_CARD)
            card["exists"] = any(
                mapping.get("url") == card["url"] for mapping in MAPPINGS
            )
            self._json(card)
        else:
            self._json({"error": "not found"}, 404)

    def do_POST(self):
        global HOSTED_INDEX
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
        elif path == "/api/webui/upload":
            if "yoto_session=stub" not in self.headers.get("Cookie", ""):
                self._json({"error": "unauthorized"}, 401)
                return
            if not body:
                self._json({"error": "web UI file is empty"}, 400)
                return
            HOSTED_INDEX = body
            self._json({"ok": True})
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
            try:
                request = json.loads(body or b"{}")
            except json.JSONDecodeError:
                request = {}
            LAST_CARD["url"] = request.get("url", "")
            LAST_CARD["seq"] += 1
            self._json({"ok": True, "seq": LAST_CARD["seq"]})
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
                request = json.loads(body or b"{}")
            except json.JSONDecodeError:
                request = {}
            target = request.get("path", "")
            recursive = request.get("recursive") is True
            is_directory = target in TREE
            has_children = any(
                candidate.startswith(target + "/") for candidate in TREE
            )
            if is_directory and has_children and not recursive:
                self._json({"error": "directory must be empty"}, 409)
                return
            targets = {target}
            if recursive and is_directory:
                targets.update(
                    candidate for candidate in TREE
                    if candidate.startswith(target + "/")
                )
            for parent, items in TREE.items():
                items[:] = [
                    item for item in items
                    if not any(
                        item["path"] == candidate
                        or item["path"].startswith(candidate + "/")
                        for candidate in targets
                    )
                ]
            for candidate in targets:
                TREE.pop(candidate, None)
            for image_path in list(IMAGES):
                if any(
                    image_path == candidate
                    or image_path.startswith(candidate + "/")
                    for candidate in targets
                ):
                    IMAGES.pop(image_path)
            self._json({"ok": True})
        elif path == "/api/delete-all":
            MAPPINGS.clear()
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
