"""Shared asyncio venue protocol client.

Wire framing: [4-byte magic 0x56454E31 "VEN1"][4-byte big-endian uint32
payload length][JSON payload]. Matches engine/order-matching-engine/venue/
framing.h exactly.

"""

from __future__ import annotations

import asyncio
import json
import struct
from typing import Awaitable, Callable, Union

MAGIC = 0x56454E31
HEADER_SIZE = 8
MAX_PAYLOAD_BYTES = 1 << 20  # matches kMaxPayloadBytes in framing.h

Handler = Callable[[dict], Union[None, Awaitable[None]]]


class FramingError(Exception):
    """A structural framing violation — bad magic or an over-length frame.

    Distinct from an application-level REJECT: this means the byte stream
    itself is corrupt, not that the venue disliked a message.
    """


def encode_frame(obj: dict) -> bytes:
    payload = json.dumps(obj).encode()
    return struct.pack(">II", MAGIC, len(payload)) + payload


class VenueClient:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self._reader = reader
        self._writer = writer
        self._handlers: dict[str, list[Handler]] = {}
        self._recv_task: asyncio.Task | None = None
        self._waiters: dict[str, list[asyncio.Future]] = {}
        self.closed = False
        self.error: Exception | None = None

    @classmethod
    async def connect(cls, host: str = "127.0.0.1", port: int = 9200) -> "VenueClient":
        reader, writer = await asyncio.open_connection(host, port)
        return cls(reader, writer)

    def on(self, msg_type: str, handler: Handler) -> None:
        """Register a callback for one message type, or "*" for every
        message. Handler may be a plain function or a coroutine function."""
        self._handlers.setdefault(msg_type, []).append(handler)

    def start(self) -> None:
        """Launch the background receive-loop task.

        Call after registering handlers with on() — starting first would
        risk dispatching messages before any listener exists.
        """
        if self._recv_task is None:
            self._recv_task = asyncio.create_task(self._receive_loop())

    async def send(self, obj: dict) -> None:
        self._writer.write(encode_frame(obj))
        await self._writer.drain()

    async def wait_for(self, msg_type: str, timeout: float = 2.0) -> dict | None:
        """One-off async wait for the next message of a given type.

        Works whether or not start() has been called — if the receive
        loop isn't running yet, this drives it directly.
        """
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._waiters.setdefault(msg_type, []).append(fut)
        if self._recv_task is None:
            self.start()
        try:
            return await asyncio.wait_for(fut, timeout)
        except asyncio.TimeoutError:
            return None
        finally:
            waiters = self._waiters.get(msg_type)
            if waiters and fut in waiters:
                waiters.remove(fut)

    async def close(self) -> None:
        if self._recv_task is not None:
            self._recv_task.cancel()
            try:
                await self._recv_task
            except (asyncio.CancelledError, Exception):
                pass
        self._writer.close()
        try:
            await self._writer.wait_closed()
        except (ConnectionError, OSError):
            pass
        self.closed = True

    async def _read_frame(self) -> dict:
        header = await self._reader.readexactly(HEADER_SIZE)
        magic, length = struct.unpack(">II", header)
        if magic != MAGIC:
            raise FramingError(f"bad frame magic 0x{magic:08x}")
        if length > MAX_PAYLOAD_BYTES:
            raise FramingError(f"frame length {length} exceeds max {MAX_PAYLOAD_BYTES}")
        payload = await self._reader.readexactly(length)
        return json.loads(payload)

    async def _receive_loop(self) -> None:
        try:
            while True:
                msg = await self._read_frame()
                await self._dispatch(msg)
        except asyncio.CancelledError:
            raise
        except Exception as e:  # noqa: BLE001 - store and stop, no reconnection logic here
            self.error = e
        finally:
            self.closed = True
            self._writer.close()

    async def _dispatch(self, msg: dict) -> None:
        msg_type = msg.get("type")
        for fut in self._waiters.pop(msg_type, []):
            if not fut.done():
                fut.set_result(msg)
        for handler in self._handlers.get(msg_type, []):
            result = handler(msg)
            if asyncio.iscoroutine(result):
                await result
        for handler in self._handlers.get("*", []):
            result = handler(msg)
            if asyncio.iscoroutine(result):
                await result
