from __future__ import annotations

import json
import threading
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

from .config import ENCDB_DATABASES_DIR

_audit_lock = threading.Lock()


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def audit_path(edb_id: int) -> Path:
    return ENCDB_DATABASES_DIR / f"edb_{edb_id}" / "audit.jsonl"


def append_audit(
    *,
    edb_id: Optional[int],
    session_id: Optional[str],
    client_id: Optional[int],
    role: str,
    action: str,
    target: str = "",
    status: str,
    latency_ms: Optional[float] = None,
    detail: str = "",
) -> dict[str, Any]:
    entry = {
        "id": str(uuid.uuid4()),
        "time": _now_iso(),
        "edb_id": edb_id,
        "session_id": session_id,
        "client_id": client_id,
        "role": role,
        "action": action,
        "target": target,
        "status": status,
        "latency_ms": latency_ms,
        "detail": detail,
        "source": "web-gateway",
    }

    path = audit_path(edb_id) if edb_id is not None else ENCDB_DATABASES_DIR / "audit_global.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps(entry, ensure_ascii=False)

    with _audit_lock:
        with path.open("a", encoding="utf-8") as f:
            f.write(line + "\n")

    return entry


def load_audit_logs(edb_id: Optional[int], limit: int) -> list[dict[str, Any]]:
    if edb_id is None:
        paths = sorted(ENCDB_DATABASES_DIR.glob("edb_*/audit.jsonl"))
        paths.append(ENCDB_DATABASES_DIR / "audit_global.jsonl")
    else:
        paths = [audit_path(edb_id)]

    entries: list[dict[str, Any]] = []
    for path in paths:
        if not path.is_file():
            continue
        with path.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    entries.append(json.loads(line))
                except json.JSONDecodeError:
                    continue

    entries.sort(key=lambda item: item.get("time", ""), reverse=True)
    return entries[:limit]
