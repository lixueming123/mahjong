"""TCP / NDJSON 网络层（UI_UX_SPEC §1.2「网络 I/O」、§11「net_client」）。

冻结约束：

- 独立读取线程 + `queue.Queue`，**主循环非阻塞**；
- 发送走同一 socket，由独立写线程 drain 一个有界队列（上限 64 帧，超限丢弃并记日志）；
- 读线程只做「读行 + UTF-8 解码 + JSON 解析 + `queue.put`」；
- 单行 > 64 KB（65536 字节）→ 主动断开（PROTOCOL §1.2 第 3 条）；
- 解析失败的行丢弃并计入坏帧（客户端侧最多记日志，不主动断开，服务端有 10 次配额）。
"""

from __future__ import annotations

import json
import logging
import queue
import select
import socket
import threading
import time

log = logging.getLogger("mahjong.client.net")

MAX_LINE = 65536
OUTBOX_MAX = 64
MAX_OUTBOUND_BYTES = 4096


class NetClient:
    """一个连接最多承载 1 个玩家（PROTOCOL §1.4）。"""

    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        # ("frame", dict) | ("connected", None) | ("connect_failed", str) | ("closed", str)
        self.inbox: queue.Queue[tuple[str, object]] = queue.Queue()
        self.outbox: queue.Queue[bytes | None] = queue.Queue(maxsize=OUTBOX_MAX)
        self._sock: socket.socket | None = None
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self.connected = False
        self.connecting = False
        self.bad_frames = 0
        self.dropped_outbound = 0
        self.sent_frames = 0
        self.recv_frames = 0
        self.last_inbound: float | None = None
        self.closed_reason: str | None = None

    # ------------------------------------------------------------ 生命周期

    def connect_async(self, timeout: float = 3.0) -> None:
        """非阻塞连接：三次握手在后台线程完成，主循环立刻返回（§1.2）。"""
        if self.connecting or self.connected:
            return
        self.connecting = True
        self._stop.clear()
        thread = threading.Thread(
            target=self._connect_worker, args=(timeout,), name="client-connect", daemon=True
        )
        self._threads.append(thread)
        thread.start()

    def _connect_worker(self, timeout: float) -> None:
        try:
            sock = socket.create_connection((self.host, self.port), timeout=timeout)
        except OSError as exc:
            self.connecting = False
            log.warning("连接 %s:%s 失败：%s", self.host, self.port, exc)
            self.inbox.put(("connect_failed", str(exc)))
            return
        if self._stop.is_set():
            sock.close()
            self.connecting = False
            return
        sock.settimeout(None)
        self._sock = sock
        self.connected = True
        self.connecting = False
        self.last_inbound = time.monotonic()
        reader = threading.Thread(target=self._read_worker, name="client-read", daemon=True)
        writer = threading.Thread(target=self._write_worker, name="client-write", daemon=True)
        self._threads.extend([reader, writer])
        reader.start()
        writer.start()
        log.info("已连接 %s:%s", self.host, self.port)
        self.inbox.put(("connected", None))

    def close(self, reason: str = "客户端主动断开") -> None:
        self._stop.set()
        sock = self._sock
        self._sock = None
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
            self.outbox.put_nowait(None)
        except queue.Full:
            pass
        self.connected = False
        self.connecting = False
        self.closed_reason = reason

    def join(self, timeout: float = 1.0) -> None:
        current = threading.current_thread()
        for thread in list(self._threads):
            if thread is current:
                continue
            thread.join(timeout=timeout)

    # ------------------------------------------------------------ 发送

    def send(self, frame: dict) -> bool:
        """入队一帧（非阻塞）。返回是否成功入队。"""
        if not self.connected or self._sock is None:
            log.debug("未连接，丢弃出站帧：%s", frame.get("type"))
            return False
        try:
            data = (json.dumps(frame, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        except (TypeError, ValueError) as exc:
            log.error("出站帧无法序列化：%s (%s)", frame.get("type"), exc)
            return False
        if len(data) > MAX_OUTBOUND_BYTES:
            log.warning("出站帧超过 4 KB：type=%s len=%d", frame.get("type"), len(data))
        try:
            self.outbox.put_nowait(data)
        except queue.Full:
            self.dropped_outbound += 1
            log.warning("发送队列已满（%d），丢弃出站帧：%s", OUTBOX_MAX, frame.get("type"))
            return False
        return True

    def _write_worker(self) -> None:
        while not self._stop.is_set():
            try:
                item = self.outbox.get(timeout=0.2)
            except queue.Empty:
                continue
            if item is None:
                break
            sock = self._sock
            if sock is None:
                break
            try:
                sock.sendall(item)
                self.sent_frames += 1
            except OSError as exc:
                self._finish(f"发送失败：{exc}")
                return

    # ------------------------------------------------------------ 接收

    def _read_worker(self) -> None:
        sock = self._sock
        buffer = bytearray()
        while not self._stop.is_set() and sock is not None:
            try:
                ready, _, _ = select.select([sock], [], [], 0.2)
            except (OSError, ValueError):
                break
            if not ready:
                continue
            try:
                chunk = sock.recv(8192)
            except OSError as exc:
                self._finish(f"读取错误：{exc}")
                return
            if not chunk:
                self._finish("服务端关闭了连接")
                return
            buffer.extend(chunk)
            while True:
                index = buffer.find(b"\n")
                if index < 0:
                    if len(buffer) > MAX_LINE:
                        self._finish("接收缓冲区超过 64 KB 仍未见到换行")
                        return
                    break
                line = bytes(buffer[:index])
                del buffer[: index + 1]
                if len(line) > MAX_LINE:
                    self._finish("收到超过 64 KB 的单帧")
                    return
                self._handle_line(line)

    def _handle_line(self, line: bytes) -> None:
        line = line.rstrip(b"\r")
        if not line:
            return
        try:
            text = line.decode("utf-8")
        except UnicodeDecodeError:
            self.bad_frames += 1
            log.warning("收到非法 UTF-8 帧，已丢弃（坏帧 %d）", self.bad_frames)
            return
        try:
            frame = json.loads(text)
        except json.JSONDecodeError as exc:
            self.bad_frames += 1
            log.warning("收到非法 JSON 帧，已丢弃（坏帧 %d）：%s", self.bad_frames, exc)
            return
        if not isinstance(frame, dict) or not isinstance(frame.get("type"), str):
            self.bad_frames += 1
            log.warning("收到缺少字符串 type 的帧，已丢弃（坏帧 %d）", self.bad_frames)
            return
        self.recv_frames += 1
        self.last_inbound = time.monotonic()
        self.inbox.put(("frame", frame))

    def _finish(self, reason: str) -> None:
        if self._stop.is_set():
            return
        self._stop.set()
        self.connected = False
        self.closed_reason = reason
        sock = self._sock
        self._sock = None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
        log.info("连接结束：%s", reason)
        self.inbox.put(("closed", reason))
