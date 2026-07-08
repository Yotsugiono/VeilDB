import os
from pathlib import Path

ENCDB_HOST = os.getenv("ENCDB_HOST", "127.0.0.1")
ENCDB_PORT = int(os.getenv("ENCDB_PORT", "9000"))

# INSERT 时从此目录读取 doc_id 对应文件（与 Client.cpp raw_doc_dir 一致）
_default_data = Path(__file__).resolve().parents[2] / "EncDB" / "data" / "enron"
ENCDB_DATA_DIR = Path(os.getenv("ENCDB_DATA_DIR", str(_default_data)))

_default_uploads = Path(__file__).resolve().parent / "uploads"
ENCDB_UPLOAD_DIR = Path(os.getenv("ENCDB_UPLOAD_DIR", str(_default_uploads)))

DOC_ID_MIN = int(os.getenv("DOC_ID_MIN", "1"))
DOC_ID_MAX = int(os.getenv("DOC_ID_MAX", "10000"))

# encdb_server 从 System_high 目录启动，使用 ../Databases → EncDB/Databases
_default_db = Path(__file__).resolve().parents[2] / "EncDB" / "Databases"
ENCDB_DATABASES_DIR = Path(os.getenv("ENCDB_DATABASES_DIR", str(_default_db)))

# 网关 HTTP
HTTP_HOST = os.getenv("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("HTTP_PORT", "8080"))

# 会话：client_id 从 1 递增
SESSION_CLIENT_ID_START = int(os.getenv("SESSION_CLIENT_ID_START", "100"))

# Audit log query limit
AUDIT_LOG_LIMIT = int(os.getenv("AUDIT_LOG_LIMIT", "500"))
