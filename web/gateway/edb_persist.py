from __future__ import annotations

import json
from pathlib import Path
from typing import List, Optional, Set, TypedDict

from .config import ENCDB_DATABASES_DIR


class DatabaseStatus(TypedDict):
    edb_id: int
    doc_count: int
    revision: int
    has_catalog: bool


def catalog_path(edb_id: int) -> Path:
    return ENCDB_DATABASES_DIR / f"edb_{edb_id}" / "catalog.json"


def save_catalog(edb_id: int, occupied: Set[int], revision: int = 0) -> None:
    path = catalog_path(edb_id)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "occupied": sorted(occupied),
        "revision": revision,
    }
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def load_catalog(edb_id: int) -> Optional[dict]:
    path = catalog_path(edb_id)
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None


def list_persisted_edb_ids() -> List[int]:
    root = ENCDB_DATABASES_DIR
    if not root.is_dir():
        return []
    ids: List[int] = []
    for child in root.iterdir():
        if not child.is_dir() or not child.name.startswith("edb_"):
            continue
        try:
            edb_id = int(child.name[4:])
        except ValueError:
            continue
        if (child / "context.dat").is_file():
            ids.append(edb_id)
    return sorted(ids)


def get_database_statuses() -> List[DatabaseStatus]:
    statuses: List[DatabaseStatus] = []
    for edb_id in list_persisted_edb_ids():
        catalog = load_catalog(edb_id)
        has_catalog = catalog is not None
        if catalog:
            occupied = catalog.get("occupied", [])
            doc_count = len(occupied) if isinstance(occupied, list) else 0
            revision = int(catalog.get("revision", 0))
        else:
            docs_dir = ENCDB_DATABASES_DIR / f"edb_{edb_id}" / "docs"
            doc_count = sum(1 for path in docs_dir.glob("*.bin")) if docs_dir.is_dir() else 0
            revision = 0
        statuses.append(
            {
                "edb_id": edb_id,
                "doc_count": doc_count,
                "revision": revision,
                "has_catalog": has_catalog,
            }
        )
    return statuses
