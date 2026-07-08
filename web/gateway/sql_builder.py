"""
将类 SQL 字符串解析为 EncDBRequest（逻辑对齐 Client.cpp + QueryCodec.h）。
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from enum import Enum, auto
from typing import List, Optional

from .encdb_rpc import (
    ENC_KEY_SIZE,
    MAX_DOC_SIZE,
    OP_DELETE,
    OP_INSERT,
    OP_REPLACE,
    UPDATE_LEGACY,
    UPDATE_SPLIT,
    BoolOp,
    EncDBRequest,
    EncTerm,
    InitRequest,
    INIT_MODE_CREATE,
    INIT_MODE_RESUME,
    RequestType,
    SelectRequest,
    UpdateRequest,
    normalize_index_bytes,
)


def tokenize_query(text: str) -> List[str]:
    tokens: List[str] = []
    current: List[str] = []

    def flush() -> None:
        if current:
            tokens.append("".join(current))
            current.clear()

    for ch in text:
        if ch.isspace():
            flush()
        elif ch in "()":
            flush()
            tokens.append(ch)
        else:
            current.append(ch)
    flush()
    return tokens


class ASTNodeType(Enum):
    TERM = auto()
    AND = auto()
    OR = auto()
    NOT = auto()


@dataclass
class ASTNode:
    type: ASTNodeType
    term_index: int = 0
    left: Optional["ASTNode"] = None
    right: Optional["ASTNode"] = None


class BoolExprParser:
    def __init__(self, tokens: List[str]) -> None:
        self._tokens = tokens
        self._pos = 0
        self.keywords: List[str] = []

    def parse_expr(self) -> ASTNode:
        node = self._parse_term()
        while self._match("OR"):
            node = ASTNode(ASTNodeType.OR, left=node, right=self._parse_term())
        return node

    def _parse_term(self) -> ASTNode:
        node = self._parse_unary()
        while self._match("AND"):
            node = ASTNode(ASTNodeType.AND, left=node, right=self._parse_unary())
        return node

    def _parse_unary(self) -> ASTNode:
        if self._match("NOT"):
            return ASTNode(ASTNodeType.NOT, left=self._parse_unary())
        return self._parse_factor()

    def _parse_factor(self) -> ASTNode:
        if self._match("("):
            node = self.parse_expr()
            self._expect(")")
            return node
        return self._parse_word()

    def _parse_word(self) -> ASTNode:
        if self._pos >= len(self._tokens):
            raise ValueError("unexpected end of expression")
        word = self._tokens[self._pos]
        self._pos += 1
        if word not in self.keywords:
            self.keywords.append(word)
        term_index = self.keywords.index(word)
        return ASTNode(ASTNodeType.TERM, term_index=term_index)

    def _match(self, token: str) -> bool:
        if self._pos < len(self._tokens) and self._tokens[self._pos].upper() == token:
            self._pos += 1
            return True
        return False

    def _expect(self, token: str) -> None:
        if not self._match(token):
            raise ValueError(f"expected {token}")


def encode_bool_expr(node: Optional[ASTNode]) -> bytes:
    if node is None:
        return b""
    out = bytearray()

    def walk(n: Optional[ASTNode]) -> None:
        if n is None:
            return
        walk(n.left)
        walk(n.right)
        if n.type == ASTNodeType.TERM:
            out.append(BoolOp.OP_TERM)
            out.append(n.term_index & 0xFF)
        elif n.type == ASTNodeType.AND:
            out.append(BoolOp.OP_AND)
        elif n.type == ASTNodeType.OR:
            out.append(BoolOp.OP_OR)
        elif n.type == ASTNodeType.NOT:
            out.append(BoolOp.OP_NOT)

    walk(node)
    return bytes(out)


def make_term(keyword: str) -> EncTerm:
    raw = keyword.encode("utf-8")[:32]
    return EncTerm(data=raw, length=len(raw))


def parse_select_type_from_sql(sql: str) -> int:
    raw = tokenize_query(sql.strip())
    if not raw or raw[0].upper() != "SELECT":
        return 0
    upper = [t.upper() for t in raw]
    select_type, _ = parse_select_type(upper)
    return select_type


def parse_select_type(tokens: List[str]) -> tuple[int, int]:
    """返回 (select_type, expr_begin_index)。"""
    if len(tokens) > 1 and tokens[1].upper() in ("MAX", "MIN", "SUM", "AVG"):
        mapping = {"MAX": 1, "MIN": 2, "SUM": 3, "AVG": 4}
        return mapping[tokens[1].upper()], 2
    return 0, 1


def build_update_request(
    client_id: int,
    op: int,
    doc_id: int,
    *,
    flags: int = UPDATE_SPLIT,
    index_content: bytes = b"",
    doc_content: bytes = b"",
) -> EncDBRequest:
    if flags == UPDATE_SPLIT:
        index_payload = normalize_index_bytes(index_content)
    else:
        index_payload = b""
    return EncDBRequest(
        client_id=client_id,
        req_type=RequestType.UPDATE,
        update=UpdateRequest(
            op=op,
            flags=flags,
            doc_id=doc_id,
            index_content=index_payload,
            doc_content=doc_content[:MAX_DOC_SIZE],
        ),
    )


def build_request_from_sql(
    sql: str,
    client_id: int,
    *,
    enc_key: Optional[bytes] = None,
    doc_content: Optional[bytes] = None,
    doc_id: Optional[int] = None,
    is_insert: bool = True,
    init_mode: int = INIT_MODE_CREATE,
    target_edb_id: int = 0,
) -> EncDBRequest:
    tokens = [t.upper() if t not in ("(", ")") else t for t in tokenize_query(sql.strip())]
    # 关键词本身在 SELECT 表达式里会由 parser 按原 token 处理；命令用大写
    raw_tokens = tokenize_query(sql.strip())
    if not raw_tokens:
        return EncDBRequest(client_id=client_id, req_type=RequestType.INVALID)

    cmd = raw_tokens[0].upper()

    if cmd == "INIT":
        key = enc_key or (b"\0" * ENC_KEY_SIZE)
        return EncDBRequest(
            client_id=client_id,
            req_type=RequestType.INIT,
            init=InitRequest(
                key=key[:ENC_KEY_SIZE].ljust(ENC_KEY_SIZE, b"\0"),
                mode=init_mode,
                target_edb_id=target_edb_id,
            ),
        )

    if cmd in ("INSERT", "DELETE"):
        if len(raw_tokens) < 2:
            raise ValueError("INSERT/DELETE requires doc_id")
        did = doc_id if doc_id is not None else int(raw_tokens[1])
        if cmd == "INSERT":
            if not doc_content:
                raise ValueError("INSERT requires doc_content")
            return build_update_request(
                client_id,
                OP_INSERT,
                did,
                flags=UPDATE_LEGACY,
                doc_content=doc_content,
            )
        return build_update_request(
            client_id,
            OP_DELETE,
            did,
            flags=UPDATE_SPLIT,
            index_content=b"",
            doc_content=b"",
        )

    if cmd == "SELECT":
        select_type, expr_begin = parse_select_type([t.upper() for t in raw_tokens])
        expr_tokens = raw_tokens[expr_begin:]
        parser = BoolExprParser(expr_tokens)
        root = parser.parse_expr()
        terms = [make_term(k) for k in parser.keywords]
        return EncDBRequest(
            client_id=client_id,
            req_type=RequestType.SELECT,
            select=SelectRequest(
                terms=terms,
                bool_expr=encode_bool_expr(root),
                select_type=select_type,
            ),
        )

    return EncDBRequest(client_id=client_id, req_type=RequestType.INVALID)
