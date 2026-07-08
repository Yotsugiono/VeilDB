"""将 EncDB SELECT 响应转换为 Web/API 友好结构。"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Any, Dict, List, Optional

from .config import ENCDB_DATA_DIR
from .encdb_rpc import MAX_DOC_SIZE, SelectResponse

_AGG_RE = re.compile(
    r"ENCDB_AGG\|op=(?P<op>[A-Z]+)\|match_count=(?P<match>\d+)\|value=(?P<value>-?\d+)"
)

SELECT_TYPE_NAMES = {0: "SELECT", 1: "MAX", 2: "MIN", 3: "SUM", 4: "AVG"}


def decode_cstr_bytes(chunk: bytes) -> str:
    """
    与 C++ std::string((char*)buf) 一致：以首个 \\0 为结尾。
    使用 latin-1 避免邮件二进制导致 UTF-8 解码失败。
    """
    if not chunk:
        return ""
    end = chunk.find(b"\0")
    if end == 0:
        return ""
    if end > 0:
        chunk = chunk[:end]
    return chunk.decode("latin-1", errors="replace")


def sanitize_for_json(text: str) -> str:
    """去掉 \\0 等控制字符，避免 JSON/浏览器截断显示。"""
    if not text:
        return ""
    # 去掉 C0 控制符，保留换行制表
    cleaned = "".join(
        ch for ch in text if ch in "\n\r\t" or (ord(ch) >= 32 and ord(ch) != 127)
    )
    return cleaned.strip()


def load_dataset_preview(doc_id: int, limit: int = 500) -> str:
    """演示兜底：密文解密结果为空时，从本地数据集读取明文（与 INSERT 同源）。"""
    path = ENCDB_DATA_DIR / str(doc_id)
    if not path.is_file():
        return ""
    try:
        raw = path.read_bytes()
        end = raw.find(b"\0")
        if end > 0:
            raw = raw[:end]
        text = raw.decode("latin-1", errors="replace")
        text = sanitize_for_json(text)
        if len(text) > limit:
            return text[:limit] + "…"
        return text
    except OSError:
        return ""


def parse_aggregate_meta(text: str) -> Optional[Dict[str, Any]]:
    text = sanitize_for_json(text)
    if not text:
        return None

    m = _AGG_RE.search(text)
    if m:
        return {
            "op": m.group("op"),
            "match_count": int(m.group("match")),
            "value": int(m.group("value")),
        }

    # 宽松解析（防止正则因杂字节失败）
    parts: Dict[str, str] = {}
    for seg in text.split("|"):
        if "=" in seg:
            k, v = seg.split("=", 1)
            parts[k.strip()] = v.strip()
    if parts.get("op") and "match_count" in parts and "value" in parts:
        try:
            return {
                "op": parts["op"],
                "match_count": int(parts["match_count"]),
                "value": int(parts["value"]),
            }
        except ValueError:
            return None
    return None


def format_select_response(
    sel: SelectResponse,
    select_type: int,
    sql: str,
) -> Dict[str, Any]:
    if select_type >= 1 and select_type <= 4:
        raw_meta = ""
        if sel.doc_contents:
            raw = sel.doc_contents[0]
            if isinstance(raw, bytes):
                raw_meta = decode_cstr_bytes(raw)
            else:
                raw_meta = raw or ""
        raw_meta = sanitize_for_json(raw_meta)

        parsed = parse_aggregate_meta(raw_meta)
        value = sel.doc_ids[0] if sel.doc_ids else 0
        match_count = 0
        op = SELECT_TYPE_NAMES.get(select_type, "AGG")
        if parsed:
            value = parsed["value"]
            match_count = parsed["match_count"]
            op = parsed["op"]

        summary = f"{op} = {value}（匹配 {match_count} 篇文档）"
        return {
            "result_mode": "aggregate",
            "aggregate_op": op,
            "match_count": match_count,
            "aggregate_value": value,
            "doc_count": 1,
            "hits": [
                {
                    "doc_id": value,
                    "preview": summary,
                    "is_aggregate": True,
                }
            ],
            "sql": sql,
        }

    hits: List[Dict[str, Any]] = []
    contents = sel.doc_contents or []
    for i, did in enumerate(sel.doc_ids):
        raw = contents[i] if i < len(contents) else b""
        if isinstance(raw, str):
            text = sanitize_for_json(raw)
        elif isinstance(raw, bytes):
            text = sanitize_for_json(decode_cstr_bytes(raw))
        else:
            text = ""

        if not text and did > 0:
            fallback = load_dataset_preview(did)
            if fallback:
                text = fallback

        if did <= 0 and not text:
            continue

        preview = text[:500] + ("…" if len(text) > 500 else "")
        hits.append(
            {
                "doc_id": did,
                "preview": preview,
                "is_aggregate": False,
            }
        )

    return {
        "result_mode": "rows",
        "aggregate_op": None,
        "match_count": len(hits),
        "aggregate_value": None,
        "doc_count": len(hits),
        "hits": hits,
        "sql": sql,
    }
