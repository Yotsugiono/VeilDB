"""
EncDB HTTP 网关 — INIT / INSERT / UPLOAD / REPLACE / DELETE / SELECT
"""
from __future__ import annotations

import secrets
import uuid
from pathlib import Path
from typing import Dict, List, Optional

from fastapi import FastAPI, File, Form, HTTPException, Request, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from .bridge import call_encdb
from .audit import append_audit, load_audit_logs
from .config import (
    AUDIT_LOG_LIMIT,
    DOC_ID_MAX,
    DOC_ID_MIN,
    ENCDB_DATA_DIR,
    ENCDB_UPLOAD_DIR,
    SESSION_CLIENT_ID_START,
)
from .edb_persist import (
    delete_persisted_edb,
    get_database_statuses,
    list_persisted_edb_ids,
    load_catalog,
    save_catalog,
)
from .encdb_rpc import (
    INIT_MODE_CREATE,
    INIT_MODE_RESUME,
    MAX_DOC_SIZE,
    OP_DELETE,
    OP_INSERT,
    OP_REPLACE,
    RequestType,
    ResponseStatus,
    UPDATE_SPLIT,
    build_shutdown_request,
)
from .response_util import format_select_response
from .sql_builder import build_request_from_sql, build_update_request
from .tokenize import build_index_payload, truncate_raw_document

app = FastAPI(title="EncDB Web Gateway", version="0.1.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

_next_client_id = SESSION_CLIENT_ID_START


class EdbDocCatalog:
    """按 edb_id 维护的内存文档 catalog（不读磁盘、不持久化）。"""

    def __init__(self) -> None:
        self.occupied: set[int] = set()
        self.revision: int = 0

    def snapshot(self) -> "DocCatalogModel":
        return DocCatalogModel(
            occupied=sorted(self.occupied),
            doc_id_min=DOC_ID_MIN,
            doc_id_max=DOC_ID_MAX,
            revision=self.revision,
        )

    def mark_occupied(self, doc_id: int) -> None:
        if doc_id not in self.occupied:
            self.revision += 1
        self.occupied.add(doc_id)

    def mark_free(self, doc_id: int) -> None:
        if doc_id in self.occupied:
            self.occupied.discard(doc_id)
            self.revision += 1

    def alloc_doc_id(self) -> int:
        for did in range(DOC_ID_MIN, DOC_ID_MAX + 1):
            if did not in self.occupied:
                return did
        raise HTTPException(
            status_code=409,
            detail=f"doc id pool exhausted ({DOC_ID_MIN}..{DOC_ID_MAX})",
        )


_edb_catalogs: Dict[int, EdbDocCatalog] = {}


def reset_edb_catalog(edb_id: int) -> EdbDocCatalog:
    """每次 INIT 成功后为该 edb_id 新建空 catalog。"""
    catalog = EdbDocCatalog()
    _edb_catalogs[edb_id] = catalog
    return catalog


def restore_edb_catalog(edb_id: int) -> EdbDocCatalog:
    catalog = EdbDocCatalog()
    saved = load_catalog(edb_id)
    if saved:
        catalog.occupied = set(int(x) for x in saved.get("occupied", []))
        catalog.revision = int(saved.get("revision", 0))
    _edb_catalogs[edb_id] = catalog
    return catalog


def get_edb_catalog(edb_id: int) -> EdbDocCatalog:
    catalog = _edb_catalogs.get(edb_id)
    if catalog is None:
        raise HTTPException(status_code=500, detail=f"catalog missing for edb_id={edb_id}")
    return catalog


class SessionState:
    def __init__(self, client_id: int, enc_key: bytes) -> None:
        self.client_id = client_id
        self.enc_key = enc_key
        self.edb_id: Optional[int] = None
        self.initialized = False

    def catalog_snapshot(self) -> "DocCatalogModel":
        if self.edb_id is None:
            return DocCatalogModel()
        return get_edb_catalog(self.edb_id).snapshot()

    def mark_occupied(self, doc_id: int) -> None:
        if self.edb_id is None:
            return
        get_edb_catalog(self.edb_id).mark_occupied(doc_id)

    def mark_free(self, doc_id: int) -> None:
        if self.edb_id is None:
            return
        get_edb_catalog(self.edb_id).mark_free(doc_id)

    def alloc_doc_id(self) -> int:
        if self.edb_id is None:
            raise HTTPException(status_code=400, detail="session not bound to edb_id")
        return get_edb_catalog(self.edb_id).alloc_doc_id()


_sessions: Dict[str, SessionState] = {}


def _alloc_client_id() -> int:
    global _next_client_id
    cid = _next_client_id
    _next_client_id += 1
    return cid


def _actor_role(request: Request) -> str:
    role = request.headers.get("X-EncDB-Role", "unknown")
    return role if role in {"admin", "user"} else "unknown"


def _safe_audit(**kwargs) -> None:
    try:
        append_audit(**kwargs)
    except Exception as exc:
        print(f"[audit] failed to write audit log: {exc}")


class DocCatalogModel(BaseModel):
    occupied: List[int] = Field(default_factory=list)
    doc_id_min: int = DOC_ID_MIN
    doc_id_max: int = DOC_ID_MAX
    revision: int = 0


class InitSessionRequest(BaseModel):
    edb_id: Optional[int] = Field(
        default=None,
        description="指定则唤醒已落盘数据库；省略则创建新库",
    )


class InitResponseModel(BaseModel):
    session_id: str
    client_id: int
    edb_id: int
    doc_catalog: DocCatalogModel
    latency_ms: float


class InsertRequest(BaseModel):
    session_id: str
    doc_id: str = Field(..., description="数据集文件名，如 1、2")


class InsertResponseModel(BaseModel):
    success: bool
    doc_id: int
    latency_ms: float
    message: str = ""


class DeleteRequest(BaseModel):
    session_id: str
    doc_id: str = Field(..., description="要删除的文档 ID")


class DeleteResponseModel(BaseModel):
    success: bool
    doc_id: int
    latency_ms: float
    message: str = ""


class QueryRequest(BaseModel):
    session_id: str
    sql: str = Field(..., examples=["SELECT firm OR name", "SELECT MAX firm AND enron"])


class QueryHit(BaseModel):
    doc_id: int
    preview: str
    is_aggregate: bool = False


class QueryResponseModel(BaseModel):
    result_mode: str = Field(..., description="rows | aggregate")
    doc_count: int
    hits: List[QueryHit]
    latency_ms: float
    sql: str
    aggregate_op: Optional[str] = None
    match_count: Optional[int] = None
    aggregate_value: Optional[int] = None


class SessionInfo(BaseModel):
    session_id: str
    client_id: int
    edb_id: Optional[int]
    initialized: bool
    doc_catalog: Optional[DocCatalogModel] = None


class ShutdownRequestModel(BaseModel):
    session_id: str


class ShutdownResponseModel(BaseModel):
    success: bool
    edb_id: Optional[int]
    latency_ms: float
    message: str = ""


class DatabaseStatusModel(BaseModel):
    edb_id: int
    doc_count: int
    revision: int
    has_catalog: bool


class DatabaseListResponse(BaseModel):
    edb_ids: List[int]
    databases: List[DatabaseStatusModel] = Field(default_factory=list)


class DeleteDatabaseResponse(BaseModel):
    success: bool
    edb_id: int
    message: str = ""


class UploadResponseModel(BaseModel):
    success: bool
    doc_id: int
    keyword_count: int
    raw_bytes: int
    truncated_index: bool = False
    truncated_raw: bool = False
    latency_ms: float
    message: str = ""
    replaced: bool = False


class HealthResponse(BaseModel):
    ok: bool
    encdb_host: str
    encdb_port: int
    data_dir: str
    data_dir_exists: bool
    upload_dir: str
    upload_dir_exists: bool


class AuditEntryModel(BaseModel):
    id: str
    time: str
    edb_id: Optional[int] = None
    session_id: Optional[str] = None
    client_id: Optional[int] = None
    role: str = "unknown"
    action: str
    target: str = ""
    status: str
    latency_ms: Optional[float] = None
    detail: str = ""
    source: str = "web-gateway"


class AuditListResponse(BaseModel):
    logs: List[AuditEntryModel]


def _require_session(session_id: str) -> SessionState:
    state = _sessions.get(session_id)
    if not state or not state.initialized:
        raise HTTPException(status_code=400, detail="call /api/session/init first")
    return state


def _session_bound_to_edb(edb_id: int) -> bool:
    return any(state.initialized and state.edb_id == edb_id for state in _sessions.values())


def _write_upload_sidecar(doc_id: int, raw: bytes, index_csv: str) -> None:
    ENCDB_UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    (ENCDB_UPLOAD_DIR / f"{doc_id}.raw").write_bytes(raw)
    (ENCDB_UPLOAD_DIR / f"{doc_id}.idx").write_text(index_csv, encoding="utf-8")


def _remove_upload_sidecar(doc_id: int) -> None:
    for suffix in (".raw", ".idx"):
        path = ENCDB_UPLOAD_DIR / f"{doc_id}{suffix}"
        if path.is_file():
            path.unlink()


@app.get("/api/health", response_model=HealthResponse)
def health() -> HealthResponse:
    from .config import ENCDB_HOST, ENCDB_PORT

    ENCDB_UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
    return HealthResponse(
        ok=True,
        encdb_host=ENCDB_HOST,
        encdb_port=ENCDB_PORT,
        data_dir=str(ENCDB_DATA_DIR),
        data_dir_exists=ENCDB_DATA_DIR.is_dir(),
        upload_dir=str(ENCDB_UPLOAD_DIR),
        upload_dir_exists=ENCDB_UPLOAD_DIR.is_dir(),
    )


@app.get("/api/databases", response_model=DatabaseListResponse)
def list_databases() -> DatabaseListResponse:
    statuses = [DatabaseStatusModel(**item) for item in get_database_statuses()]
    return DatabaseListResponse(edb_ids=[item.edb_id for item in statuses], databases=statuses)


@app.delete("/api/databases/{edb_id}", response_model=DeleteDatabaseResponse)
def delete_database(request: Request, edb_id: int) -> DeleteDatabaseResponse:
    role = _actor_role(request)
    if role != "admin":
        _safe_audit(
            edb_id=None,
            session_id=None,
            client_id=None,
            role=role,
            action="删除数据库",
            target=f"数据库 #{edb_id}",
            status="error",
            detail="admin role required",
        )
        raise HTTPException(status_code=403, detail="admin role required")
    if edb_id <= 0:
        raise HTTPException(status_code=400, detail="invalid edb_id")
    if edb_id not in list_persisted_edb_ids():
        _safe_audit(
            edb_id=None,
            session_id=None,
            client_id=None,
            role=role,
            action="删除数据库",
            target=f"数据库 #{edb_id}",
            status="error",
            detail="database not found",
        )
        raise HTTPException(status_code=404, detail=f"edb_id={edb_id} not found on disk")
    if _session_bound_to_edb(edb_id):
        _safe_audit(
            edb_id=None,
            session_id=None,
            client_id=None,
            role=role,
            action="删除数据库",
            target=f"数据库 #{edb_id}",
            status="error",
            detail="database is bound to an active session",
        )
        raise HTTPException(status_code=409, detail="database is in use; save and disconnect first")

    try:
        delete_persisted_edb(edb_id)
    except FileNotFoundError:
        raise HTTPException(status_code=404, detail=f"edb_id={edb_id} not found on disk") from None
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    _edb_catalogs.pop(edb_id, None)
    _safe_audit(
        edb_id=None,
        session_id=None,
        client_id=None,
        role=role,
        action="删除数据库",
        target=f"数据库 #{edb_id}",
        status="success",
        detail="persistent database directory deleted",
    )
    return DeleteDatabaseResponse(success=True, edb_id=edb_id, message="database deleted")


@app.post("/api/session/init", response_model=InitResponseModel)
def session_init(request: Request, body: Optional[InitSessionRequest] = None) -> InitResponseModel:
    role = _actor_role(request)
    enc_key = secrets.token_bytes(16)
    client_id = _alloc_client_id()
    session_id = str(uuid.uuid4())
    _sessions[session_id] = SessionState(client_id=client_id, enc_key=enc_key)

    resume_edb_id = body.edb_id if body and body.edb_id is not None else None
    action = "连接数据库" if resume_edb_id is not None else "创建数据库"
    if resume_edb_id is not None:
        if resume_edb_id not in list_persisted_edb_ids():
            _safe_audit(
                edb_id=None,
                session_id=session_id,
                client_id=client_id,
                role=role,
                action=action,
                target=f"数据库 #{resume_edb_id}",
                status="error",
                detail=f"edb_id={resume_edb_id} not found on disk",
            )
            raise HTTPException(status_code=404, detail=f"edb_id={resume_edb_id} not found on disk")
        req = build_request_from_sql(
            "INIT",
            client_id,
            enc_key=enc_key,
            init_mode=INIT_MODE_RESUME,
            target_edb_id=resume_edb_id,
        )
    else:
        req = build_request_from_sql("INIT", client_id, enc_key=enc_key)

    resp, latency_ms = call_encdb(req, client_id)
    audit_latency = round(latency_ms, 2)

    if resp.status != ResponseStatus.OK or resp.init is None:
        _safe_audit(
            edb_id=resume_edb_id,
            session_id=session_id,
            client_id=client_id,
            role=role,
            action=action,
            target=f"数据库 #{resume_edb_id}" if resume_edb_id is not None else "新数据库",
            status="error",
            latency_ms=audit_latency,
            detail="INIT failed on encdb_server",
        )
        raise HTTPException(status_code=502, detail="INIT failed on encdb_server")

    state = _sessions[session_id]
    state.edb_id = resp.init.edb_id
    state.initialized = True
    if resume_edb_id is not None:
        restore_edb_catalog(state.edb_id)
    else:
        reset_edb_catalog(state.edb_id)

    _safe_audit(
        edb_id=state.edb_id,
        session_id=session_id,
        client_id=client_id,
        role=role,
        action=action,
        target=f"数据库 #{state.edb_id}",
        status="success",
        latency_ms=audit_latency,
        detail=f"session={session_id}",
    )

    return InitResponseModel(
        session_id=session_id,
        client_id=client_id,
        edb_id=resp.init.edb_id,
        doc_catalog=state.catalog_snapshot(),
        latency_ms=audit_latency,
    )


@app.post("/api/session/shutdown", response_model=ShutdownResponseModel)
def session_shutdown(request: Request, body: ShutdownRequestModel) -> ShutdownResponseModel:
    role = _actor_role(request)
    state = _require_session(body.session_id)
    req = build_shutdown_request(state.client_id)
    resp, latency_ms = call_encdb(req, state.client_id)
    audit_latency = round(latency_ms, 2)

    ok = resp.status == ResponseStatus.OK and resp.shutdown and resp.shutdown.success == 1
    if ok and state.edb_id is not None:
        catalog = get_edb_catalog(state.edb_id)
        save_catalog(state.edb_id, catalog.occupied, catalog.revision)

    if ok:
        _sessions.pop(body.session_id, None)

    _safe_audit(
        edb_id=state.edb_id,
        session_id=body.session_id,
        client_id=state.client_id,
        role=role,
        action="保存并断开",
        target=f"数据库 #{state.edb_id}" if state.edb_id is not None else "未绑定数据库",
        status="success" if ok else "error",
        latency_ms=audit_latency,
        detail="" if ok else "shutdown flush failed",
    )

    return ShutdownResponseModel(
        success=ok,
        edb_id=state.edb_id,
        latency_ms=audit_latency,
        message="" if ok else "shutdown flush failed",
    )


@app.get("/api/session/{session_id}", response_model=SessionInfo)
def session_info(session_id: str) -> SessionInfo:
    state = _sessions.get(session_id)
    if not state:
        raise HTTPException(status_code=404, detail="session not found")
    return SessionInfo(
        session_id=session_id,
        client_id=state.client_id,
        edb_id=state.edb_id,
        initialized=state.initialized,
        doc_catalog=state.catalog_snapshot() if state.initialized else None,
    )


@app.post("/api/insert", response_model=InsertResponseModel)
def insert_doc(request: Request, body: InsertRequest) -> InsertResponseModel:
    role = _actor_role(request)
    state = _require_session(body.session_id)

    doc_path = ENCDB_DATA_DIR / body.doc_id
    if not doc_path.is_file():
        _safe_audit(
            edb_id=state.edb_id,
            session_id=body.session_id,
            client_id=state.client_id,
            role=role,
            action="插入 Enron 示例",
            target=f"文档 #{body.doc_id}",
            status="error",
            detail=f"document not found: {doc_path}",
        )
        raise HTTPException(status_code=404, detail=f"document not found: {doc_path}")

    payload = (doc_path.read_bytes() + b"\0")[:MAX_DOC_SIZE]

    req = build_request_from_sql(
        f"INSERT {body.doc_id}",
        state.client_id,
        doc_content=payload,
        doc_id=int(body.doc_id),
        is_insert=True,
    )
    resp, latency_ms = call_encdb(req, state.client_id)
    audit_latency = round(latency_ms, 2)

    ok = resp.status == ResponseStatus.OK and resp.update and resp.update.success == 1
    if ok:
        state.mark_occupied(int(body.doc_id))
    _safe_audit(
        edb_id=state.edb_id,
        session_id=body.session_id,
        client_id=state.client_id,
        role=role,
        action="插入 Enron 示例",
        target=f"文档 #{body.doc_id}",
        status="success" if ok else "error",
        latency_ms=audit_latency,
        detail="" if ok else "update failed",
    )
    return InsertResponseModel(
        success=ok,
        doc_id=int(body.doc_id),
        latency_ms=audit_latency,
        message="" if ok else "update failed",
    )


@app.post("/api/delete", response_model=DeleteResponseModel)
def delete_doc(request: Request, body: DeleteRequest) -> DeleteResponseModel:
    role = _actor_role(request)
    state = _require_session(body.session_id)
    did = int(body.doc_id)

    req = build_update_request(
        state.client_id,
        OP_DELETE,
        did,
        flags=UPDATE_SPLIT,
        index_content=b"",
        doc_content=b"",
    )
    resp, latency_ms = call_encdb(req, state.client_id)
    audit_latency = round(latency_ms, 2)

    ok = resp.status == ResponseStatus.OK and resp.update and resp.update.success == 1
    if ok:
        state.mark_free(did)
        _remove_upload_sidecar(did)
    _safe_audit(
        edb_id=state.edb_id,
        session_id=body.session_id,
        client_id=state.client_id,
        role=role,
        action="删除文档",
        target=f"文档 #{did}",
        status="success" if ok else "error",
        latency_ms=audit_latency,
        detail="" if ok else "delete failed",
    )
    return DeleteResponseModel(
        success=ok,
        doc_id=did,
        latency_ms=audit_latency,
        message="" if ok else "delete failed",
    )


@app.post("/api/upload", response_model=UploadResponseModel)
async def upload_doc(
    request: Request,
    session_id: str = Form(...),
    file: UploadFile = File(...),
    doc_id: Optional[int] = Form(None),
) -> UploadResponseModel:
    role = _actor_role(request)
    state = _require_session(session_id)
    raw = await file.read()
    raw, truncated_raw = truncate_raw_document(raw, MAX_DOC_SIZE)

    did = doc_id if doc_id is not None else state.alloc_doc_id()
    if did < DOC_ID_MIN or did > DOC_ID_MAX:
        _safe_audit(
            edb_id=state.edb_id,
            session_id=session_id,
            client_id=state.client_id,
            role=role,
            action="上传文档",
            target=f"文档 #{did}",
            status="error",
            detail=f"doc_id must be in {DOC_ID_MIN}..{DOC_ID_MAX}",
        )
        raise HTTPException(status_code=400, detail=f"doc_id must be in {DOC_ID_MIN}..{DOC_ID_MAX}")

    index_csv = build_index_payload(raw.decode("utf-8", errors="replace"))
    index_bytes = index_csv.encode("utf-8")
    truncated_index = len(index_bytes) > MAX_DOC_SIZE
    index_bytes = index_bytes[:MAX_DOC_SIZE]

    req = build_update_request(
        state.client_id,
        OP_INSERT,
        did,
        flags=UPDATE_SPLIT,
        index_content=index_bytes,
        doc_content=raw,
    )
    resp, latency_ms = call_encdb(req, state.client_id)
    audit_latency = round(latency_ms, 2)

    ok = resp.status == ResponseStatus.OK and resp.update and resp.update.success == 1
    if ok:
        state.mark_occupied(did)
        _write_upload_sidecar(did, raw, index_csv)
    _safe_audit(
        edb_id=state.edb_id,
        session_id=session_id,
        client_id=state.client_id,
        role=role,
        action="上传文档",
        target=f"文档 #{did}",
        status="success" if ok else "error",
        latency_ms=audit_latency,
        detail=f"file={file.filename or '-'}, raw_bytes={len(raw)}" if ok else "upload failed",
    )

    return UploadResponseModel(
        success=ok,
        doc_id=did,
        keyword_count=len(index_csv.split(",")) if index_csv else 0,
        raw_bytes=len(raw),
        truncated_index=truncated_index,
        truncated_raw=truncated_raw,
        latency_ms=audit_latency,
        message="" if ok else "upload failed",
        replaced=False,
    )


@app.post("/api/replace", response_model=UploadResponseModel)
async def replace_doc(
    request: Request,
    session_id: str = Form(...),
    file: UploadFile = File(...),
    doc_id: int = Form(...),
) -> UploadResponseModel:
    role = _actor_role(request)
    state = _require_session(session_id)
    raw = await file.read()
    raw, truncated_raw = truncate_raw_document(raw, MAX_DOC_SIZE)

    index_csv = build_index_payload(raw.decode("utf-8", errors="replace"))
    index_bytes = index_csv.encode("utf-8")
    truncated_index = len(index_bytes) > MAX_DOC_SIZE
    index_bytes = index_bytes[:MAX_DOC_SIZE]

    req = build_update_request(
        state.client_id,
        OP_REPLACE,
        doc_id,
        flags=UPDATE_SPLIT,
        index_content=index_bytes,
        doc_content=raw,
    )
    resp, latency_ms = call_encdb(req, state.client_id)
    audit_latency = round(latency_ms, 2)

    ok = resp.status == ResponseStatus.OK and resp.update and resp.update.success == 1
    if ok:
        state.mark_occupied(doc_id)
        _write_upload_sidecar(doc_id, raw, index_csv)
    _safe_audit(
        edb_id=state.edb_id,
        session_id=session_id,
        client_id=state.client_id,
        role=role,
        action="替换文档",
        target=f"文档 #{doc_id}",
        status="success" if ok else "error",
        latency_ms=audit_latency,
        detail=f"file={file.filename or '-'}, raw_bytes={len(raw)}" if ok else "replace failed",
    )

    return UploadResponseModel(
        success=ok,
        doc_id=doc_id,
        keyword_count=len(index_csv.split(",")) if index_csv else 0,
        raw_bytes=len(raw),
        truncated_index=truncated_index,
        truncated_raw=truncated_raw,
        latency_ms=audit_latency,
        message="" if ok else "replace failed",
        replaced=True,
    )


@app.post("/api/query", response_model=QueryResponseModel)
def run_query(request: Request, body: QueryRequest) -> QueryResponseModel:
    role = _actor_role(request)
    state = _require_session(body.session_id)

    sql = body.sql.strip()
    if not sql.upper().startswith("SELECT"):
        _safe_audit(
            edb_id=state.edb_id,
            session_id=body.session_id,
            client_id=state.client_id,
            role=role,
            action="执行查询",
            target=sql[:200],
            status="error",
            detail="only SELECT is supported",
        )
        raise HTTPException(status_code=400, detail="only SELECT is supported")

    req = build_request_from_sql(sql, state.client_id)
    if req.req_type != RequestType.SELECT or req.select is None:
        _safe_audit(
            edb_id=state.edb_id,
            session_id=body.session_id,
            client_id=state.client_id,
            role=role,
            action="执行查询",
            target=sql[:200],
            status="error",
            detail="invalid SELECT",
        )
        raise HTTPException(status_code=400, detail="invalid SELECT")

    select_type = req.select.select_type
    resp, latency_ms = call_encdb(req, state.client_id)
    audit_latency = round(latency_ms, 2)
    if resp.status != ResponseStatus.OK or resp.select is None:
        _safe_audit(
            edb_id=state.edb_id,
            session_id=body.session_id,
            client_id=state.client_id,
            role=role,
            action="执行查询",
            target=sql[:200],
            status="error",
            latency_ms=audit_latency,
            detail="query failed on encdb_server",
        )
        raise HTTPException(status_code=502, detail="query failed on encdb_server")

    formatted = format_select_response(resp.select, select_type, sql)
    hits = [QueryHit(**h) for h in formatted["hits"]]
    detail = (
        f"result_mode={formatted['result_mode']}, "
        f"doc_count={formatted['doc_count']}, "
        f"match_count={formatted.get('match_count')}"
    )
    _safe_audit(
        edb_id=state.edb_id,
        session_id=body.session_id,
        client_id=state.client_id,
        role=role,
        action="执行查询",
        target=sql[:200],
        status="success",
        latency_ms=audit_latency,
        detail=detail,
    )

    return QueryResponseModel(
        result_mode=formatted["result_mode"],
        doc_count=formatted["doc_count"],
        hits=hits,
        latency_ms=audit_latency,
        sql=sql,
        aggregate_op=formatted.get("aggregate_op"),
        match_count=formatted.get("match_count"),
        aggregate_value=formatted.get("aggregate_value"),
    )


@app.get("/api/audit/logs", response_model=AuditListResponse)
def audit_logs(edb_id: Optional[int] = None, limit: int = AUDIT_LOG_LIMIT) -> AuditListResponse:
    safe_limit = max(1, min(limit, 1000))
    logs = [AuditEntryModel(**item) for item in load_audit_logs(edb_id, safe_limit)]
    return AuditListResponse(logs=logs)


_frontend = Path(__file__).resolve().parent.parent / "frontend"
if _frontend.is_dir():
    app.mount("/static", StaticFiles(directory=str(_frontend)), name="static")


@app.get("/")
def root():
    return {
        "service": "EncDB Web Gateway",
        "ui": "/static/index.html",
        "docs": "/docs",
    }
