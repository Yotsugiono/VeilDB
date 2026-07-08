"""
与 EncDB System_high/CryptoTestingApp/RPC.cpp 对齐的二进制编解码。
TCP 线上格式: [uint32_be length][payload]，payload 即 serialize_encdb_request/response 的输出。
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import List, Optional

ENC_KEY_SIZE = 16
MAX_TERMS = 16
MAX_KEYWORD_LEN = 32
MAX_DOC_SIZE = 4096
MAX_INDEX_SIZE = MAX_DOC_SIZE
MAX_RESULT_DOCS = 100

UPDATE_LEGACY = 0
UPDATE_SPLIT = 1

OP_DELETE = 0
OP_INSERT = 1
OP_REPLACE = 2

UPDATE_WIRE_V1 = 1
UPDATE_WIRE_LEGACY_SINGLE = 2


def normalize_index_bytes(index_content: bytes) -> bytes:
    """
    Enclave wordTokenize 使用 strtok，索引平面须以 NUL 结尾（对齐 Legacy INSERT 的 doc_len+1）。
    """
    if not index_content:
        return b""
    trimmed = index_content[: MAX_INDEX_SIZE - 1]
    if trimmed.endswith(b"\0"):
        return trimmed
    return trimmed + b"\0"


INIT_MODE_CREATE = 0
INIT_MODE_RESUME = 1


class RequestType(IntEnum):
    INIT = 0
    SELECT = 1
    UPDATE = 2
    SHUTDOWN = 3
    INVALID = 4


class ResponseStatus(IntEnum):
    OK = 0
    ERROR = 1


class ResponseType(IntEnum):
    INIT_RESULT = 0
    SELECT_RESULT = 1
    UPDATE_RESULT = 2
    SHUTDOWN_RESULT = 3
    INVALID = 4


class BoolOp(IntEnum):
    OP_TERM = 0x01
    OP_AND = 0x02
    OP_OR = 0x03
    OP_NOT = 0x04


def _append_u32(buf: bytearray, v: int) -> None:
    buf.extend(struct.pack("<I", v & 0xFFFFFFFF))


def _append_i32(buf: bytearray, v: int) -> None:
    buf.extend(struct.pack("<i", v))


def _append_u8(buf: bytearray, v: int) -> None:
    buf.append(v & 0xFF)


def _read_u32(data: bytes, offset: int) -> tuple[int, int]:
    (v,) = struct.unpack_from("<I", data, offset)
    return v, offset + 4


def _read_i32(data: bytes, offset: int) -> tuple[int, int]:
    (v,) = struct.unpack_from("<i", data, offset)
    return v, offset + 4


def _read_u8(data: bytes, offset: int) -> tuple[int, int]:
    return data[offset], offset + 1


@dataclass
class EncTerm:
    data: bytes = field(default_factory=lambda: bytes(MAX_KEYWORD_LEN))
    length: int = 0


@dataclass
class InitRequest:
    key: bytes = field(default_factory=lambda: bytes(ENC_KEY_SIZE))
    mode: int = INIT_MODE_CREATE
    target_edb_id: int = 0


@dataclass
class ShutdownRequest:
    reserved: int = 0


@dataclass
class SelectRequest:
    terms: List[EncTerm] = field(default_factory=list)
    bool_expr: bytes = b""
    select_type: int = 0


@dataclass
class UpdateRequest:
    op: int = OP_INSERT
    flags: int = UPDATE_LEGACY
    doc_id: int = 0
    doc_content: bytes = b""
    index_content: bytes = b""


@dataclass
class EncDBRequest:
    client_id: int = 0
    req_type: RequestType = RequestType.INVALID
    init: Optional[InitRequest] = None
    select: Optional[SelectRequest] = None
    update: Optional[UpdateRequest] = None
    shutdown: Optional[ShutdownRequest] = None


@dataclass
class InitResponse:
    edb_id: int = 0


@dataclass
class SelectResponse:
    doc_count: int = 0
    doc_ids: List[int] = field(default_factory=list)
    doc_contents: List[bytes] = field(default_factory=list)


@dataclass
class UpdateResponse:
    success: int = 0
    doc_id: int = 0


@dataclass
class ShutdownResponse:
    success: int = 0


@dataclass
class EncDBResponse:
    status: ResponseStatus = ResponseStatus.ERROR
    resp_type: ResponseType = ResponseType.INVALID
    init: Optional[InitResponse] = None
    select: Optional[SelectResponse] = None
    update: Optional[UpdateResponse] = None
    shutdown: Optional[ShutdownResponse] = None


def serialize_request(enc: EncDBRequest, client_id: int) -> bytes:
    buf = bytearray()
    _append_u8(buf, enc.client_id & 0xFF)
    _append_u32(buf, int(enc.req_type))

    body_len_pos = len(buf)
    _append_u32(buf, 0)
    body_start = len(buf)

    if enc.req_type == RequestType.INIT and enc.init:
        key = enc.init.key[:ENC_KEY_SIZE].ljust(ENC_KEY_SIZE, b"\0")[:ENC_KEY_SIZE]
        buf.extend(key)
        _append_u8(buf, enc.init.mode)
        _append_u32(buf, enc.init.target_edb_id)
    elif enc.req_type == RequestType.SHUTDOWN:
        _append_u8(buf, 0)
    elif enc.req_type == RequestType.SELECT and enc.select:
        s = enc.select
        _append_u32(buf, len(s.terms))
        for t in s.terms:
            _append_u8(buf, t.length)
            buf.extend(t.data[: t.length])
        _append_u32(buf, len(s.bool_expr))
        buf.extend(s.bool_expr)
        _append_u8(buf, s.select_type)
    elif enc.req_type == RequestType.UPDATE and enc.update:
        u = enc.update
        _append_u8(buf, u.op)
        _append_i32(buf, u.doc_id)
        if u.flags == UPDATE_SPLIT:
            _append_u8(buf, UPDATE_WIRE_V1)
            _append_u8(buf, u.flags)
            index = u.index_content[:MAX_INDEX_SIZE]
            _append_u32(buf, len(index))
            buf.extend(index)
            doc = u.doc_content[:MAX_DOC_SIZE]
            _append_u32(buf, len(doc))
            buf.extend(doc)
        else:
            _append_u8(buf, UPDATE_WIRE_LEGACY_SINGLE)
            doc = u.doc_content[:MAX_DOC_SIZE]
            _append_u32(buf, len(doc))
            buf.extend(doc)

    body_len = len(buf) - body_start
    struct.pack_into("<I", buf, body_len_pos, body_len)
    return bytes(buf)


def deserialize_request(payload: bytes) -> EncDBRequest:
    off = 0
    client_id, off = _read_u8(payload, off)
    req_type_val, off = _read_u32(payload, off)
    body_len, off = _read_u32(payload, off)
    body_end = off + body_len

    enc = EncDBRequest(client_id=client_id, req_type=RequestType(req_type_val))

    if enc.req_type == RequestType.INIT:
        key = payload[off : off + ENC_KEY_SIZE]
        off += ENC_KEY_SIZE
        mode = INIT_MODE_CREATE
        target_edb_id = 0
        if off < body_end:
            mode, off = _read_u8(payload, off)
            if off + 4 <= body_end:
                target_edb_id, off = _read_u32(payload, off)
        enc.init = InitRequest(key=key, mode=mode, target_edb_id=target_edb_id)
    elif enc.req_type == RequestType.SHUTDOWN:
        enc.shutdown = ShutdownRequest()
    elif enc.req_type == RequestType.SELECT:
        term_count, off = _read_u32(payload, off)
        terms: List[EncTerm] = []
        for _ in range(term_count):
            ln, off = _read_u8(payload, off)
            terms.append(EncTerm(data=payload[off : off + ln], length=ln))
            off += ln
        bool_len, off = _read_u32(payload, off)
        bool_expr = payload[off : off + bool_len]
        off += bool_len
        sel_type, off = _read_u8(payload, off)
        enc.select = SelectRequest(terms=terms, bool_expr=bool_expr, select_type=sel_type)
    elif enc.req_type == RequestType.UPDATE:
        op, off = _read_u8(payload, off)
        doc_id, off = _read_i32(payload, off)
        tag_ptr = off
        wire_tag, off = _read_u8(payload, off)
        if wire_tag == UPDATE_WIRE_V1:
            flags, off = _read_u8(payload, off)
            index_len, off = _read_u32(payload, off)
            index_content = payload[off : off + index_len]
            off += index_len
            doc_len, off = _read_u32(payload, off)
            doc_content = payload[off : off + doc_len]
            enc.update = UpdateRequest(
                op=op,
                flags=flags,
                doc_id=doc_id,
                index_content=index_content,
                doc_content=doc_content,
            )
        elif wire_tag == UPDATE_WIRE_LEGACY_SINGLE:
            doc_len, off = _read_u32(payload, off)
            doc_content = payload[off : off + doc_len]
            enc.update = UpdateRequest(
                op=op,
                flags=UPDATE_LEGACY,
                doc_id=doc_id,
                doc_content=doc_content,
            )
        else:
            off = tag_ptr
            doc_len, off = _read_u32(payload, off)
            doc_content = payload[off : off + doc_len]
            enc.update = UpdateRequest(
                op=op,
                flags=UPDATE_LEGACY,
                doc_id=doc_id,
                doc_content=doc_content,
            )
    return enc


def serialize_response(enc: EncDBResponse, client_id: int) -> bytes:
    del client_id
    buf = bytearray()
    _append_u32(buf, int(enc.status))
    _append_u32(buf, int(enc.resp_type))

    body_len_pos = len(buf)
    _append_u32(buf, 0)
    body_start = len(buf)

    if enc.resp_type == ResponseType.INIT_RESULT and enc.init:
        _append_u8(buf, enc.init.edb_id)
    elif enc.resp_type == ResponseType.SELECT_RESULT and enc.select:
        s = enc.select
        _append_u32(buf, s.doc_count)
        for did in s.doc_ids:
            _append_i32(buf, did)
        for text in s.doc_contents:
            raw = text[:MAX_DOC_SIZE]
            padded = raw.ljust(MAX_DOC_SIZE, b"\0")[:MAX_DOC_SIZE]
            buf.extend(padded)
    elif enc.resp_type == ResponseType.UPDATE_RESULT and enc.update:
        _append_u32(buf, enc.update.success)
        _append_i32(buf, enc.update.doc_id)
    elif enc.resp_type == ResponseType.SHUTDOWN_RESULT and enc.shutdown:
        _append_u8(buf, enc.shutdown.success)

    body_len = len(buf) - body_start
    struct.pack_into("<I", buf, body_len_pos, body_len)
    return bytes(buf)


def deserialize_response(payload: bytes) -> EncDBResponse:
    off = 0
    status_val, off = _read_u32(payload, off)
    type_val, off = _read_u32(payload, off)
    _, off = _read_u32(payload, off)

    enc = EncDBResponse(
        status=ResponseStatus(status_val),
        resp_type=ResponseType(type_val),
    )

    if enc.resp_type == ResponseType.INIT_RESULT:
        edb_id, _ = _read_u8(payload, off)
        enc.init = InitResponse(edb_id=edb_id)
    elif enc.resp_type == ResponseType.SELECT_RESULT:
        doc_count, off = _read_u32(payload, off)
        doc_ids = []
        for _ in range(doc_count):
            did, off = _read_i32(payload, off)
            doc_ids.append(did)
        contents: List[bytes] = []
        for _ in range(doc_count):
            chunk = payload[off : off + MAX_DOC_SIZE]
            off += MAX_DOC_SIZE
            contents.append(bytes(chunk))
        enc.select = SelectResponse(
            doc_count=doc_count, doc_ids=doc_ids, doc_contents=contents
        )
    elif enc.resp_type == ResponseType.UPDATE_RESULT:
        success, off = _read_u32(payload, off)
        doc_id, off = _read_i32(payload, off)
        enc.update = UpdateResponse(success=success, doc_id=doc_id)
    elif enc.resp_type == ResponseType.SHUTDOWN_RESULT:
        success, _ = _read_u8(payload, off)
        enc.shutdown = ShutdownResponse(success=success)
    return enc


def build_init_request(
    client_id: int,
    *,
    enc_key: bytes,
    mode: int = INIT_MODE_CREATE,
    target_edb_id: int = 0,
) -> EncDBRequest:
    return EncDBRequest(
        client_id=client_id,
        req_type=RequestType.INIT,
        init=InitRequest(
            key=enc_key[:ENC_KEY_SIZE].ljust(ENC_KEY_SIZE, b"\0"),
            mode=mode,
            target_edb_id=target_edb_id,
        ),
    )


def build_shutdown_request(client_id: int) -> EncDBRequest:
    return EncDBRequest(
        client_id=client_id,
        req_type=RequestType.SHUTDOWN,
        shutdown=ShutdownRequest(),
    )
