#!/usr/bin/env python3
"""
EncDB 网关聚合 match_count 诊断脚本

用途：在 CLI 聚合结果正确的前提下，对比「TCP 原始响应」与「网关解析结果」，
定位 match_count=0 发生在哪一层。

用法见同目录 README 或脚本末尾说明。
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

# 将 web/ 加入 path，便于 import gateway.*
WEB_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WEB_ROOT))

from gateway.bridge import call_encdb  # noqa: E402
from gateway.config import ENCDB_DATA_DIR, ENCDB_HOST, ENCDB_PORT  # noqa: E402
from gateway.encdb_rpc import (  # noqa: E402
    RequestType,
    deserialize_response,
    serialize_request,
)
from gateway.response_util import (  # noqa: E402
    decode_cstr_bytes,
    format_select_response,
    parse_aggregate_meta,
    sanitize_for_json,
)
from gateway.sql_builder import build_request_from_sql  # noqa: E402


def hr(title: str) -> None:
    print("\n" + "=" * 60)
    print(title)
    print("=" * 60)


def show_raw_meta(label: str, raw: bytes) -> None:
    print(f"{label} len = {len(raw)}")
    if not raw:
        print(f"{label} is EMPTY")
        return
    preview = raw[:96]
    print(f"{label} hex[0:96] = {preview.hex()}")
    end = raw.find(b"\0")
    print(f"{label} first_nul_at = {end}")
    text = decode_cstr_bytes(raw)
    print(f"{label} decode_cstr = {text!r}")
    print(f"{label} sanitize    = {sanitize_for_json(text)!r}")
    parsed = parse_aggregate_meta(text)
    print(f"{label} parse_aggregate_meta = {parsed}")


def insert_docs(client_id: int, doc_ids: list[str]) -> None:
    for doc_id in doc_ids:
        path = ENCDB_DATA_DIR / doc_id
        if not path.is_file():
            raise FileNotFoundError(f"dataset file not found: {path}")
        from gateway.encdb_rpc import MAX_DOC_SIZE

        body = (path.read_bytes() + b"\0")[:MAX_DOC_SIZE]
        req = build_request_from_sql(
            f"INSERT {doc_id}",
            client_id,
            doc_content=body,
            doc_id=int(doc_id),
        )
        resp, ms = call_encdb(req, client_id)
        ok = resp.status.name == "OK" and resp.update and resp.update.success == 1
        print(f"  INSERT {doc_id}: ok={ok} latency={ms:.2f}ms")


def main() -> int:
    parser = argparse.ArgumentParser(description="EncDB gateway aggregate debugger")
    parser.add_argument(
        "--client-id",
        type=int,
        default=199,
        help="TCP client_id（避免与 CLI 的 1 冲突，默认 199）",
    )
    parser.add_argument(
        "--sql",
        default="SELECT MAX firm OR name",
        help="聚合 SQL（默认 SELECT MAX firm OR name）",
    )
    parser.add_argument(
        "--docs",
        default="1,2,3",
        help="先 INSERT 的 doc_id，逗号分隔（默认 1,2,3）",
    )
    parser.add_argument(
        "--skip-insert",
        action="store_true",
        help="跳过 INSERT（库中已有数据时使用）",
    )
    parser.add_argument(
        "--skip-init",
        action="store_true",
        help="跳过 INIT（已初始化且复用同一 client_id 时使用）",
    )
    args = parser.parse_args()

    client_id = args.client_id
    doc_list = [x.strip() for x in args.docs.split(",") if x.strip()]

    hr("环境")
    print(f"ENCDB_HOST      = {ENCDB_HOST}")
    print(f"ENCDB_PORT      = {ENCDB_PORT}")
    print(f"ENCDB_DATA_DIR  = {ENCDB_DATA_DIR}")
    print(f"exists          = {ENCDB_DATA_DIR.is_dir()}")
    print(f"client_id       = {client_id}")
    print(f"sql             = {args.sql}")

    if not args.skip_init:
        hr("Step 1: INIT")
        init_req = build_request_from_sql("INIT", client_id, enc_key=os.urandom(16))
        init_resp, ms = call_encdb(init_req, client_id)
        print(f"status={init_resp.status.name} edb_id={getattr(init_resp.init, 'edb_id', None)} latency={ms:.2f}ms")
        if init_resp.status.name != "OK":
            print("INIT failed, abort.")
            return 1

    if not args.skip_insert:
        hr(f"Step 2: INSERT {doc_list}")
        insert_docs(client_id, doc_list)

    hr("Step 3: 构建请求（检查 select_type）")
    req = build_request_from_sql(args.sql, client_id)
    if req.req_type != RequestType.SELECT or req.select is None:
        print("ERROR: SQL 未解析为 SELECT")
        return 1
    print(f"request.select_type = {req.select.select_type}  (MAX=1 MIN=2 SUM=3 AVG=4)")
    if req.select.select_type == 0:
        print("WARNING: select_type=0，这是普通查询而非聚合，请检查 SQL 写法")

    hr("Step 4: TCP 往返 + 反序列化")
    wire_req = serialize_request(req, client_id)
    print(f"request wire bytes = {len(wire_req)}")

    resp, latency_ms = call_encdb(req, client_id)
    print(f"response status   = {resp.status.name}")
    print(f"response type     = {resp.resp_type.name}")
    print(f"latency           = {latency_ms:.2f} ms")

    if resp.select is None:
        print("ERROR: 无 select 响应体")
        return 1

    sel = resp.select
    print(f"doc_count         = {sel.doc_count}")
    print(f"doc_ids           = {sel.doc_ids}")

    hr("Step 5: doc_contents[0] 原始字节（应与 CLI meta 一致）")
    if not sel.doc_contents:
        print("doc_contents 列表为空 —— 这就是 match_count=0 的直接原因")
    else:
        show_raw_meta("doc_contents[0]", sel.doc_contents[0])

    hr("Step 6: format_select_response（与 /api/query 相同逻辑）")
    formatted = format_select_response(sel, req.select.select_type, args.sql)
    print(f"result_mode       = {formatted.get('result_mode')}")
    print(f"aggregate_op      = {formatted.get('aggregate_op')}")
    print(f"match_count       = {formatted.get('match_count')}")
    print(f"aggregate_value   = {formatted.get('aggregate_value')}")
    print(f"hits[0].preview   = {formatted.get('hits', [{}])[0].get('preview', '')!r}")

    hr("结论提示")
    raw0 = sel.doc_contents[0] if sel.doc_contents else b""
    text = sanitize_for_json(decode_cstr_bytes(raw0))
    if b"ENCDB_AGG" in raw0[:128] and "match_count=" in text:
        if formatted.get("match_count", 0) > 0:
            print("网关解析正常。若网页仍为 0，检查浏览器 /api/query JSON 或是否旧 uvicorn 进程。")
        else:
            print("原始 meta 正常但 parse 失败 —— 把 Step 5 的 decode/sanitize 输出发给开发者。")
    elif b"ENCDB_AGG" in raw0[:128]:
        print("字节里有 ENCDB_AGG 但 decode 后丢失 —— 检查 \\0 位置与 decode_cstr_bytes。")
    else:
        print("网关收到的 doc_contents[0] 无 ENCDB_AGG。")
        print("CLI 正常而本脚本无 meta → 多半连的不是同一 encdb_server，或 client_id/库不一致。")
        print("请用与网页相同的 client_id 或在同一 session 流程下对比。")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
