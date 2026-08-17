"""Minimal frame-aware venue client

Wire framing: [4-byte magic 0x56454E31 "VEN1"][4-byte big-endian uint32 payload
length][JSON payload].
"""

import json
import select
import socket
import struct
import time

MAGIC = 0x56454E31


def encode_frame(obj: dict) -> bytes:
    payload = json.dumps(obj).encode()
    return struct.pack(">II", MAGIC, len(payload)) + payload


class VenueClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9200, timeout: float = 2.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setblocking(False)
        self.buf = b""
        self.msgs: list[dict] = []

    def send(self, obj: dict) -> None:
        self.sock.sendall(encode_frame(obj))

    def send_raw(self, data: bytes) -> None:
        """Send arbitrary bytes (used to inject a malformed frame payload)."""
        self.sock.sendall(data)

    def drain(self) -> None:
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                self.buf += chunk
        except (BlockingIOError, InterruptedError):
            pass
        while len(self.buf) >= 8:
            magic, length = struct.unpack(">II", self.buf[:8])
            if magic != MAGIC:
                raise ValueError(f"bad frame magic 0x{magic:08x}")
            if len(self.buf) < 8 + length:
                break
            payload = self.buf[8 : 8 + length]
            self.buf = self.buf[8 + length :]
            self.msgs.append(json.loads(payload))

    def pump(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.sock], [], [], 0.05)
            if ready:
                self.drain()

    def wait_for(self, msg_type: str, seconds: float = 2.0) -> dict | None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.pump(0.05)
            for m in self.msgs:
                if m.get("type") == msg_type:
                    return m
        return None

    def of_type(self, msg_type: str) -> list[dict]:
        return [m for m in self.msgs if m.get("type") == msg_type]

    def types(self) -> list[str]:
        return [m.get("type") for m in self.msgs]

    def hard_reset_close(self) -> None:
        """Force an RST (abrupt kill) rather than a graceful FIN."""
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.sock.close()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass
