"""
TCP 桥接 encdb_server — 与 net.cpp 相同的长度前缀帧。
"""
from __future__ import annotations

import socket
import struct
from typing import Tuple

from .config import ENCDB_HOST, ENCDB_PORT
from .encdb_rpc import EncDBRequest, EncDBResponse, deserialize_response, serialize_request


def _send_frame(sock: socket.socket, payload: bytes) -> None:
    header = struct.pack(">I", len(payload))
    sock.sendall(header + payload)


def _recv_frame(sock: socket.socket) -> bytes:
    header = _recv_exact(sock, 4)
    (length,) = struct.unpack(">I", header)
    if length == 0:
        return b""
    return _recv_exact(sock, length)


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("encdb_server closed connection")
        buf.extend(chunk)
    return bytes(buf)


def call_encdb(req: EncDBRequest, client_id: int) -> Tuple[EncDBResponse, float]:
    """
    单次请求-响应。返回 (响应, 往返毫秒)。
    """
    import time

    payload = serialize_request(req, client_id)
    t0 = time.perf_counter()
    with socket.create_connection((ENCDB_HOST, ENCDB_PORT), timeout=120) as sock:
        _send_frame(sock, payload)
        resp_payload = _recv_frame(sock)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    return deserialize_response(resp_payload), elapsed_ms
