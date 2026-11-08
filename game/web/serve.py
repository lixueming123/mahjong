#!/usr/bin/env python3
"""麻雀 Web 客户端的本地服务：静态站点 + WebSocket -> TCP 转发。

为什么需要这一层：服务端（`game/server`，PROTOCOL.md §1）只讲裸 TCP + NDJSON，
浏览器拿不到裸 socket，所以这里做一个最小的桥：

    浏览器 --ws://127.0.0.1:8080/ws--> 本脚本 --tcp 127.0.0.1:9527--> mahjong_server

桥只搬字节，不解析协议（除了按 `\\n` 切帧），因此协议的唯一权威仍是
PROTOCOL.md 与 C++ 服务端。客户端侧的帧处理全在 `js/client.js`。

用法：

    python3 game/web/serve.py                     # 站点 :8080，转发到 :9527
    python3 game/web/serve.py --http-port 9000 --game-port 19527
    python3 game/web/serve.py --game-host 10.0.0.5 --game-port 9527

只依赖标准库（handshake 与 WS 帧编解码手写），无需 pip install。
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

# Share the frozen wire enum table with the game clients (game/protocol/enums.json).
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol.enums import ERROR_CODE, FRAME_TYPE

WEB_ROOT = os.path.dirname(os.path.abspath(__file__))
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".mjs": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".svg": "image/svg+xml",
    ".ico": "image/x-icon",
    ".wav": "audio/wav",
    ".mp3": "audio/mpeg",
    ".woff2": "font/woff2",
    ".map": "application/json; charset=utf-8",
}


def log(*args: object) -> None:
    print("[bridge]", *args, file=sys.stderr, flush=True)


# ------------------------------------------------------------------ WebSocket 编解码

def ws_accept_key(key: str) -> str:
    digest = hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def read_exact(stream, count: int) -> bytes | None:
    buf = b""
    while len(buf) < count:
        chunk = stream.read(count - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def encode_ws_frame(opcode: int, payload: bytes) -> bytes:
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length < 65536:
        header.append(126)
        header += struct.pack(">H", length)
    else:
        header.append(127)
        header += struct.pack(">Q", length)
    return bytes(header) + payload


def read_ws_frame(stream) -> tuple[int, bytes] | None:
    """读一帧（已合并分片）。返回 (opcode, payload)；对端关闭返回 None。"""
    fragments = bytearray()
    first_opcode = None
    while True:
        head = read_exact(stream, 2)
        if head is None:
            return None
        fin = bool(head[0] & 0x80)
        opcode = head[0] & 0x0F
        masked = bool(head[1] & 0x80)
        length = head[1] & 0x7F
        if length == 126:
            ext = read_exact(stream, 2)
            if ext is None:
                return None
            length = struct.unpack(">H", ext)[0]
        elif length == 127:
            ext = read_exact(stream, 8)
            if ext is None:
                return None
            length = struct.unpack(">Q", ext)[0]
        mask_key = read_exact(stream, 4) if masked else None
        if masked and mask_key is None:
            return None
        payload = read_exact(stream, length) if length else b""
        if payload is None:
            return None
        if masked:
            payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
        if opcode in (0x0, 0x1, 0x2):
            if opcode != 0x0:
                first_opcode = opcode
            fragments += payload
            if fin:
                return first_opcode or 0x1, bytes(fragments)
            continue
        return opcode, payload


# ------------------------------------------------------------------ 一条 WS 连接 = 一条 TCP 连接

class BridgeSession:
    def __init__(self, handler, game_host: str, game_port: int) -> None:
        self.handler = handler
        self.rfile = handler.rfile
        self.wfile = handler.wfile
        self.write_lock = threading.Lock()
        self.closed = threading.Event()
        self.tcp: socket.socket | None = None
        try:
            self.tcp = socket.create_connection((game_host, game_port), timeout=5.0)
            self.tcp.settimeout(None)
        except OSError as exc:
            log(f"连接游戏服务端 {game_host}:{game_port} 失败：{exc}")
            self.send(
                0x1,
                json.dumps(
                    {
                        "type": FRAME_TYPE.ERROR,
                        "code": ERROR_CODE.INTERNAL,
                        "message": "cannot reach mahjong server",
                    },
                    ensure_ascii=False,
                ).encode(),
            )
            self.send(0x8, b"")
            self.closed.set()

    # -------------------------------------------------- 写
    def send(self, opcode: int, payload: bytes) -> None:
        if self.closed.is_set():
            return
        try:
            with self.write_lock:
                self.wfile.write(encode_ws_frame(opcode, payload))
                self.wfile.flush()
        except (OSError, ValueError):
            self.closed.set()

    def send_text(self, text: str) -> None:
        self.send(0x1, text.encode("utf-8"))

    # -------------------------------------------------- 主循环
    def run(self) -> None:
        if self.tcp is None:
            return
        tcp_reader = threading.Thread(target=self.pump_tcp, name="bridge-tcp", daemon=True)
        tcp_reader.start()
        try:
            self.pump_ws()
        finally:
            self.shutdown()
            tcp_reader.join(timeout=1.0)

    def pump_ws(self) -> None:
        """浏览器 -> 服务端。WS 文本帧 = 一帧 NDJSON（补 `\\n`）。"""
        while not self.closed.is_set():
            frame = read_ws_frame(self.rfile)
            if frame is None:
                return
            opcode, payload = frame
            if opcode == 0x8:  # close
                self.send(0x8, b"")
                return
            if opcode == 0x9:  # ping
                self.send(0xA, payload)
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode not in (0x1, 0x2):
                continue
            text = payload.decode("utf-8", "replace").strip()
            if not text:
                continue
            if self.tcp is None:
                return
            try:
                self.tcp.sendall(text.encode("utf-8") + b"\n")
            except OSError:
                return

    def pump_tcp(self) -> None:
        """服务端 -> 浏览器。按 `\\n` 切帧，每行一个 WS 文本帧。"""
        assert self.tcp is not None
        buf = b""
        while not self.closed.is_set():
            try:
                chunk = self.tcp.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                self.send_text(line.decode("utf-8", "replace"))
        self.closed.set()
        self.send(0x8, b"")

    def shutdown(self) -> None:
        self.closed.set()
        sock, self.tcp = self.tcp, None
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass
        try:
            self.handler.close_connection = True
        except OSError:
            pass


# ------------------------------------------------------------------ HTTP

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mahjong-web-bridge/1.0"

    # 由 make_server 注入
    game_host = "127.0.0.1"
    game_port = 9527

    def log_message(self, fmt: str, *args: object) -> None:  # 安静一点
        if "/ws" in (fmt % args if args else fmt):
            return

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path in ("/ws", "/ws/"):
            self.handle_ws()
            return
        self.serve_static(path)

    def handle_ws(self) -> None:
        upgrade = (self.headers.get("Upgrade") or "").lower()
        key = self.headers.get("Sec-WebSocket-Key")
        if upgrade != "websocket" or not key:
            self.send_error(400, "expected websocket upgrade")
            return
        accept = ws_accept_key(key)
        self.wfile.write(
            (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
            ).encode("ascii")
        )
        self.wfile.flush()
        self.close_connection = True
        session = BridgeSession(self, self.game_host, self.game_port)
        session.run()

    def serve_static(self, path: str) -> None:
        if path in ("", "/"):
            path = "/index.html"
        # 只允许落在 web 目录下：先归一化，再拒绝任何越界
        relative = os.path.normpath(path.lstrip("/")).replace(os.sep, "/")
        if relative.startswith("..") or relative.startswith("/"):
            self.send_error(403, "forbidden")
            return
        full = os.path.join(WEB_ROOT, relative)
        if not os.path.isfile(full):
            self.send_error(404, "not found")
            return
        try:
            with open(full, "rb") as handle:
                body = handle.read()
        except OSError:
            self.send_error(500, "read error")
            return
        ext = os.path.splitext(full)[1].lower()
        self.send_response(200)
        self.send_header("Content-Type", CONTENT_TYPES.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass


def make_server(http_port: int, game_host: str, game_port: int) -> ThreadingHTTPServer:
    handler = type("BoundHandler", (Handler,), {"game_host": game_host, "game_port": game_port})
    server = ThreadingHTTPServer(("127.0.0.1", http_port), handler)
    server.daemon_threads = True
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description="麻雀 Web 客户端站点 + WebSocket→TCP 桥")
    parser.add_argument("--http-port", type=int, default=8080, help="静态站点端口（默认 8080）")
    parser.add_argument("--game-host", default="127.0.0.1", help="游戏服务端地址（默认 127.0.0.1）")
    parser.add_argument("--game-port", "--port", dest="game_port", type=int, default=9527,
                        help="游戏服务端端口（默认 9527，即 PROTOCOL.md §1.1 的冻结值）")
    args = parser.parse_args()

    server = make_server(args.http_port, args.game_host, args.game_port)
    log(f"http://127.0.0.1:{args.http_port}/  ->  ws://127.0.0.1:{args.http_port}/ws"
        f"  ->  tcp {args.game_host}:{args.game_port}")
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        log("bye")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
