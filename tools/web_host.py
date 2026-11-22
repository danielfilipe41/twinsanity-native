#!/usr/bin/env python3
"""Serves a web build (`PLATFORM=web`, `build/web`) so a player can host their own disc image beside it and just play.

The page (web/shell.html) probes for disc.iso/disc.bin next to itself and starts the game with the image it finds. This
server answers that probe and serves the image with Range requests, which is what lets the page read the disc as the
game asks for its bytes instead of copying a multi-gigabyte image into the browser first. `--iso` points at an image
anywhere on disk (the player's documents folder, say) and exposes it as disc.iso without copying it.

    python tools/web_host.py                                  # build/web, with disc.iso/disc.bin beside the page
    python tools/web_host.py --iso ~/Documents/game.iso       # that image, at any path
    python tools/web_host.py --root build/web --port 8765 --bind 0.0.0.0

Any static host does as well, as long as it answers Range requests and sends game.wasm as application/wasm: nginx and
Apache do, `python -m http.server` does for Ranges but sends .wasm as application/octet-stream, which the browser
refuses to compile as a module (this server sends the right types).
"""
import argparse
import mimetypes
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_ROOT = ROOT / "build" / "web"

# The types a browser will only use when it is told: a wasm module needs application/wasm, and a script's type decides
# whether it runs as a module or not
TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".wasm": "application/wasm",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json",
    ".png": "image/png",
    ".iso": "application/octet-stream",
    ".bin": "application/octet-stream",
    ".data": "application/octet-stream",
}
# The page and the module it loads are rebuilt often: a cached copy would run last build's game against this build's wasm
UNCACHED = {".html", ".js", ".mjs", ".wasm"}


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.1, so a connection is reused: the page asks for the disc a 2 MB chunk at a time, and under HTTP/1.0 every one
    # of those paid a fresh connection's handshake (the transport bench read 4.5 ms/MB of it). Keep-alive is also what any
    # real host of this page does, so a local run matches one
    protocol_version = "HTTP/1.1"
    root = DEFAULT_ROOT
    image = None

    def ContentType(self, path: Path) -> str:
        return TYPES.get(path.suffix.lower()) or mimetypes.guess_type(path.name)[0] or "application/octet-stream"

    def Resolve(self):
        """The file a request names, or None. disc.iso/disc.bin are the image --iso named, wherever it lives."""
        path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
        if self.image is not None and path in ("/disc.iso", "/disc.bin"):
            return self.image
        resolved = (self.root / path.lstrip("/")).resolve()
        if self.root not in resolved.parents and resolved != self.root:
            return None
        if resolved.is_dir():
            index = resolved / "index.html"
            resolved = index if index.is_file() else resolved / "game.html"
        return resolved if resolved.is_file() else None

    def Headers(self, status: int, path: Path, size: int, start: int, end: int):
        self.send_response(status)
        self.send_header("Content-Type", self.ContentType(path))
        self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Content-Length", str(end - start + 1))
        else:
            self.send_header("Content-Length", str(size))
        if path.suffix.lower() in UNCACHED:
            self.send_header("Cache-Control", "no-cache")
        # ?nostore=1 asks for a response the browser may not keep: the page's transport bench compares a request whose
        # response can be written to the HTTP cache against one that cannot, which is a header only the server can send
        if urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get("nostore"):
            self.send_header("Cache-Control", "no-store")
        self.end_headers()

    def Range(self, size: int):
        """The byte range asked for: (start, end), or None when there is no usable Range header, or (None, None) when it
        cannot be answered (a range past the file's end)."""
        header = self.headers.get("Range")
        if not header or not header.startswith("bytes=") or size == 0:
            return None
        first, _, last = header[len("bytes="):].split(",")[0].strip().partition("-")
        try:
            if first == "":
                length = int(last)
                if length <= 0:
                    return None
                start, end = max(0, size - length), size - 1
            else:
                start = int(first)
                end = int(last) if last else size - 1
        except ValueError:
            return None
        end = min(end, size - 1)
        if start > end or start >= size:
            return (None, None)
        return (start, end)

    def do_HEAD(self):
        path = self.Resolve()
        if path is None:
            self.send_error(404)
            return
        size = os.path.getsize(path)
        self.Headers(200, path, size, 0, size - 1)

    def do_GET(self):
        path = self.Resolve()
        if path is None:
            self.send_error(404)
            return
        size = os.path.getsize(path)
        asked = self.Range(size)
        if asked == (None, None):
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        start, end = asked if asked else (0, size - 1)
        status = 206 if asked else 200
        self.Headers(status, path, size, start, end)
        remaining = end - start + 1
        try:
            with open(path, "rb") as file:
                file.seek(start)
                while remaining > 0:
                    chunk = file.read(min(1 << 20, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            # The page navigated away or the game stopped reading: nothing left to send
            pass

    def do_POST(self):
        # A run's log, sent by the page's own button: a phone's log panel cannot be copied out by hand mid-run, and the frame
        # numbers a slow picture is judged by are in it. The body is what the page had in the log, written beside the build
        if urllib.parse.urlparse(self.path).path != "/report":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length > 0 else b""
        target = Path(self.server.report_path)
        with open(target, "w", encoding="utf-8", errors="replace") as file:
            file.write(body.decode("utf-8", "replace"))
        sys.stderr.write(f"report: {len(body)} bytes from {self.client_address[0]} -> {target}\n")
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, format, *args):
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), format % args))


def FindImage(root: Path, asked):
    """The disc image the page will find: the one asked for, or the one already sitting beside the page."""
    if asked is not None:
        image = Path(asked).expanduser().resolve()
        if not image.is_file():
            raise SystemExit(f"--iso {asked}: no file there")
        return image
    for name in ("disc.iso", "disc.bin"):
        candidate = root / name
        if candidate.is_file():
            return candidate.resolve()
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=str(DEFAULT_ROOT), help=f"the web build to serve (default {DEFAULT_ROOT})")
    parser.add_argument("--iso", default=None, help="the disc image to play, exposed as disc.iso (default: disc.iso/disc.bin beside the page)")
    parser.add_argument("--port", type=int, default=8765, help="port to listen on (default 8765)")
    parser.add_argument("--bind", default="127.0.0.1", help="address to bind, 0.0.0.0 to let others play (default 127.0.0.1)")
    args = parser.parse_args()

    root = Path(args.root).expanduser().resolve()
    if not (root / "game.html").is_file():
        raise SystemExit(f"{root} has no game.html: build the page with `DATA=runtime PLATFORM=web python3 configure.py`"
                         " and `ninja game` in this tree")
    handler = type("BoundHandler", (Handler,), {"root": root, "image": FindImage(root, args.iso)})

    server = ThreadingHTTPServer((args.bind, args.port), handler)
    # Where a run's log lands when the page's button sends it (Handler.do_POST)
    server.report_path = root / "report.txt"
    shown = "localhost" if args.bind in ("127.0.0.1", "0.0.0.0") else args.bind
    print(f"serving {root}")
    if handler.image is not None:
        print(f"disc: {handler.image} ({handler.image.stat().st_size // 1048576} MB), read with Range requests as the game asks")
    else:
        print("disc: none yet -- put disc.iso/disc.bin beside the page, or pass --iso, to skip the picker")
    print(f"play at http://{shown}:{args.port}/game.html")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
