import hashlib
import json
import os
import re
import sqlite3
import threading
from contextlib import contextmanager
from pathlib import Path
from urllib.parse import quote, unquote

from fastapi import FastAPI, Header, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field


DATA_ROOT = Path(os.environ.get("MSEED_DATA_ROOT", "/var/lib/mseed-server")).resolve()
PART_DIR = DATA_ROOT / "parts"
FILE_DIR = DATA_ROOT / "files"
DB_PATH = DATA_ROOT / "mseed.db"
UPLOAD_TOKEN = os.environ.get("MSEED_UPLOAD_TOKEN", "")
DOWNLOAD_TOKEN = os.environ.get("MSEED_DOWNLOAD_TOKEN", "")
CONTENT_RANGE_RE = re.compile(r"^bytes (\d+)-(\d+)/(\d+)$")
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9_-]+$")
MAX_CHUNK_SIZE = 2 * 1024 * 1024
LEGACY_DEVICE_ID = "01"

app = FastAPI(title="MiniCollection mseed server", version="2.0.0")
write_lock = threading.Lock()


class AlgorithmParameters(BaseModel):
    tri_on: float
    tri_off: float
    nsta: int
    nlta: int
    detect_ch: int
    qualified_ch: int
    energy_thre: float
    amp_thre: float
    intrach: int
    interch: int


class DeviceConfigRequest(BaseModel):
    parameters: AlgorithmParameters
    upload_server_url: str = ""


class DeviceRequest(BaseModel):
    name: str = ""


class StationStatus(BaseModel):
    station_number: int
    connected: bool
    online_channels: int
    total_channels: int
    gps: int


class DeviceHeartbeat(BaseModel):
    program_running: bool = True
    program_version: str = ""
    network_state: str = ""
    local_ip: str = ""
    applied_config_version: int = 0
    stations: list[StationStatus] = Field(default_factory=list)


class ConfigAppliedRequest(BaseModel):
    version: int
    success: bool = True
    message: str = ""


def safe_name(raw_name: str) -> str:
    name = unquote(raw_name).strip()
    if not name or name in {".", ".."} or Path(name).name != name:
        raise HTTPException(status_code=400, detail="invalid file name")
    if not name.lower().endswith(".mseed"):
        raise HTTPException(status_code=400, detail="only .mseed files are accepted")
    return name


def safe_device_id(raw_device_id: str) -> str:
    device_id = raw_device_id.strip()
    if not device_id or not DEVICE_ID_RE.fullmatch(device_id):
        raise HTTPException(status_code=400, detail="invalid device id")
    return device_id


def request_device_id(query_value: str | None, header_value: str | None) -> str:
    if query_value and header_value and query_value != header_value:
        raise HTTPException(status_code=400, detail="device id mismatch")
    return safe_device_id(query_value or header_value or LEGACY_DEVICE_ID)


def device_part_dir(device_id: str) -> Path:
    path = PART_DIR / device_id
    path.mkdir(parents=True, exist_ok=True)
    return path


def device_file_dir(device_id: str) -> Path:
    path = FILE_DIR / device_id
    path.mkdir(parents=True, exist_ok=True)
    return path


def require_token(actual: str | None, expected: str, kind: str) -> None:
    if expected and actual != expected:
        raise HTTPException(status_code=401, detail=f"invalid {kind} token")


@contextmanager
def database():
    conn = sqlite3.connect(DB_PATH, timeout=30)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_storage() -> None:
    PART_DIR.mkdir(parents=True, exist_ok=True)
    FILE_DIR.mkdir(parents=True, exist_ok=True)
    with database() as conn:
        conn.execute("PRAGMA journal_mode=WAL")
        columns = {
            row["name"] for row in conn.execute("PRAGMA table_info(files)").fetchall()
        }
        if columns and "device_id" not in columns:
            conn.execute("ALTER TABLE files RENAME TO files_legacy")
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS files (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                name TEXT NOT NULL,
                size INTEGER NOT NULL,
                sha256 TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                UNIQUE(device_id, name)
            )
            """
        )
        if columns and "device_id" not in columns:
            conn.execute(
                """
                INSERT INTO files(id, device_id, name, size, sha256, created_at)
                SELECT id, ?, name, size, sha256, created_at FROM files_legacy
                """,
                (LEGACY_DEVICE_ID,),
            )
            conn.execute("DROP TABLE files_legacy")
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS devices (
                device_id TEXT PRIMARY KEY,
                name TEXT NOT NULL DEFAULT '',
                enabled INTEGER NOT NULL DEFAULT 1,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                last_seen_at TEXT,
                last_upload_at TEXT,
                remote_ip TEXT NOT NULL DEFAULT '',
                status_json TEXT NOT NULL DEFAULT '{}',
                applied_config_version INTEGER NOT NULL DEFAULT 0
            )
            """
        )
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS device_configs (
                device_id TEXT NOT NULL,
                version INTEGER NOT NULL,
                parameters_json TEXT NOT NULL,
                upload_server_url TEXT NOT NULL DEFAULT '',
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                pulled_at TEXT,
                applied_at TEXT,
                apply_status TEXT NOT NULL DEFAULT 'pending',
                apply_message TEXT NOT NULL DEFAULT '',
                PRIMARY KEY(device_id, version)
            )
            """
        )
        config_columns = {
            row["name"]
            for row in conn.execute("PRAGMA table_info(device_configs)").fetchall()
        }
        if "pulled_at" not in config_columns:
            conn.execute("ALTER TABLE device_configs ADD COLUMN pulled_at TEXT")
        conn.execute(
            """INSERT OR IGNORE INTO devices(device_id)
               SELECT DISTINCT device_id FROM files"""
        )

    # Upgrade the legacy flat layout after the database transaction succeeds.
    legacy_files = [path for path in FILE_DIR.iterdir() if path.is_file()]
    legacy_parts = [path for path in PART_DIR.iterdir() if path.is_file()]
    for source, destination_dir in (
        *((path, device_file_dir(LEGACY_DEVICE_ID)) for path in legacy_files),
        *((path, device_part_dir(LEGACY_DEVICE_ID)) for path in legacy_parts),
    ):
        destination = destination_dir / source.name
        if destination.exists():
            if destination.stat().st_size != source.stat().st_size:
                raise RuntimeError(f"migration collision: {destination}")
            source.unlink()
        else:
            os.replace(source, destination)


@app.on_event("startup")
def startup() -> None:
    init_storage()


def completed_file(device_id: str, name: str):
    with database() as conn:
        return conn.execute(
            """SELECT id, device_id, name, size, sha256, created_at
               FROM files WHERE device_id = ? AND name = ?""",
            (device_id, name),
        ).fetchone()


def ensure_device(conn: sqlite3.Connection, device_id: str) -> None:
    conn.execute("INSERT OR IGNORE INTO devices(device_id) VALUES (?)", (device_id,))


def model_dict(model: BaseModel) -> dict:
    # Pydantic 1/2 compatibility for existing and freshly installed servers.
    dump = getattr(model, "model_dump", None)
    return dump() if dump else model.dict()


@app.get("/healthz")
def healthz():
    return {"status": "ok"}


def device_payload(row: sqlite3.Row) -> dict:
    status = json.loads(row["status_json"] or "{}")
    return {
        "device_id": row["device_id"],
        "name": row["name"],
        "enabled": bool(row["enabled"]),
        "online": bool(row["online"]),
        "last_seen_at": row["last_seen_at"],
        "last_upload_at": row["last_upload_at"],
        "remote_ip": row["remote_ip"],
        "applied_config_version": row["applied_config_version"],
        "desired_config_version": row["desired_config_version"],
        "status": status,
    }


DEVICE_SELECT = """
    SELECT d.*,
           CASE WHEN d.last_seen_at IS NOT NULL
                     AND (julianday('now') - julianday(d.last_seen_at)) * 86400 <= 30
                THEN 1 ELSE 0 END AS online,
           COALESCE((SELECT MAX(c.version) FROM device_configs c
                     WHERE c.device_id = d.device_id), 0) AS desired_config_version
    FROM devices d
"""


@app.get("/coalmine/devices")
def list_devices():
    with database() as conn:
        rows = conn.execute(DEVICE_SELECT + " ORDER BY d.device_id").fetchall()
    return [device_payload(row) for row in rows]


@app.get("/coalmine/devices/{device_id}")
def get_device(device_id: str):
    identity = safe_device_id(device_id)
    with database() as conn:
        row = conn.execute(
            DEVICE_SELECT + " WHERE d.device_id = ?", (identity,)
        ).fetchone()
    if not row:
        raise HTTPException(status_code=404, detail="device not found")
    return device_payload(row)


@app.put("/coalmine/devices/{device_id}")
def put_device(device_id: str, payload: DeviceRequest):
    identity = safe_device_id(device_id)
    with database() as conn:
        ensure_device(conn, identity)
        conn.execute(
            """UPDATE devices SET name = ?, enabled = 1,
               updated_at = CURRENT_TIMESTAMP WHERE device_id = ?""",
            (payload.name.strip(), identity),
        )
        row = conn.execute(
            DEVICE_SELECT + " WHERE d.device_id = ?", (identity,)
        ).fetchone()
    return device_payload(row)


@app.delete("/coalmine/devices/{device_id}", status_code=204)
def disable_device(device_id: str):
    identity = safe_device_id(device_id)
    with database() as conn:
        result = conn.execute(
            """UPDATE devices SET enabled = 0, updated_at = CURRENT_TIMESTAMP
               WHERE device_id = ?""",
            (identity,),
        )
        if result.rowcount == 0:
            raise HTTPException(status_code=404, detail="device not found")
    return Response(status_code=204)


@app.put("/coalmine/devices/{device_id}/config")
def put_device_config(device_id: str, payload: DeviceConfigRequest):
    identity = safe_device_id(device_id)
    parameters = model_dict(payload.parameters)
    with write_lock, database() as conn:
        ensure_device(conn, identity)
        version = conn.execute(
            """SELECT COALESCE(MAX(version), 0) + 1 FROM device_configs
               WHERE device_id = ?""",
            (identity,),
        ).fetchone()[0]
        conn.execute(
            """INSERT INTO device_configs
               (device_id, version, parameters_json, upload_server_url)
               VALUES (?, ?, ?, ?)""",
            (identity, version, json.dumps(parameters), payload.upload_server_url.strip()),
        )
    return {"device_id": identity, "version": version, "status": "pending"}


@app.get("/coalmine/devices/{device_id}/config/latest")
def get_latest_device_config(
    device_id: str,
    after_version: int = Query(0, ge=0),
):
    identity = safe_device_id(device_id)
    with database() as conn:
        row = conn.execute(
            """SELECT device_id, version, parameters_json, upload_server_url,
                      created_at, pulled_at, applied_at, apply_status, apply_message
               FROM device_configs WHERE device_id = ? AND version > ?
               ORDER BY version DESC LIMIT 1""",
            (identity, after_version),
        ).fetchone()
        if not row:
            return Response(status_code=204)
        conn.execute(
            """UPDATE device_configs SET pulled_at = COALESCE(pulled_at, CURRENT_TIMESTAMP),
               apply_status = CASE WHEN apply_status = 'pending' THEN 'pulled'
                                   ELSE apply_status END
               WHERE device_id = ? AND version = ?""",
            (identity, row["version"]),
        )
    return {
        "device_id": identity,
        "version": row["version"],
        "parameters": json.loads(row["parameters_json"]),
        "upload_server_url": row["upload_server_url"],
        "created_at": row["created_at"],
    }


@app.get("/coalmine/devices/{device_id}/config/status")
def get_device_config_status(device_id: str):
    identity = safe_device_id(device_id)
    with database() as conn:
        row = conn.execute(
            """SELECT version, parameters_json, upload_server_url,
                      created_at, pulled_at, applied_at, apply_status, apply_message
               FROM device_configs WHERE device_id = ?
               ORDER BY version DESC LIMIT 1""",
            (identity,),
        ).fetchone()
    if not row:
        return Response(status_code=204)
    result = dict(row)
    result["parameters"] = json.loads(result.pop("parameters_json"))
    return result


@app.post("/coalmine/devices/{device_id}/config/applied")
def report_config_applied(device_id: str, payload: ConfigAppliedRequest):
    identity = safe_device_id(device_id)
    status = "applied" if payload.success else "failed"
    with database() as conn:
        result = conn.execute(
            """UPDATE device_configs SET applied_at = CURRENT_TIMESTAMP,
               apply_status = ?, apply_message = ?
               WHERE device_id = ? AND version = ?""",
            (status, payload.message, identity, payload.version),
        )
        if result.rowcount == 0:
            raise HTTPException(status_code=404, detail="config version not found")
        if payload.success:
            conn.execute(
                """UPDATE devices SET applied_config_version = ?,
                   updated_at = CURRENT_TIMESTAMP WHERE device_id = ?""",
                (payload.version, identity),
            )
    return {"device_id": identity, "version": payload.version, "status": status}


@app.post("/coalmine/devices/{device_id}/heartbeat")
def device_heartbeat(device_id: str, payload: DeviceHeartbeat, request: Request):
    identity = safe_device_id(device_id)
    status_json = json.dumps(model_dict(payload))
    remote_ip = request.client.host if request.client else ""
    with database() as conn:
        ensure_device(conn, identity)
        conn.execute(
            """UPDATE devices SET last_seen_at = CURRENT_TIMESTAMP, remote_ip = ?,
               status_json = ?, applied_config_version = ?, enabled = 1,
               updated_at = CURRENT_TIMESTAMP WHERE device_id = ?""",
            (remote_ip, status_json, payload.applied_config_version, identity),
        )
    return {"device_id": identity, "status": "ok"}


@app.head("/coalmine/mseed/upload")
def upload_status(
    name: str = Query(...),
    device_id: str | None = Query(default=None),
    x_device_id: str | None = Header(default=None),
    x_upload_token: str | None = Header(default=None),
):
    require_token(x_upload_token, UPLOAD_TOKEN, "upload")
    filename = safe_name(name)
    identity = request_device_id(device_id, x_device_id)
    with database() as conn:
        ensure_device(conn, identity)
    row = completed_file(identity, filename)
    if row:
        return Response(
            status_code=200,
            headers={"X-Uploaded-Bytes": str(row["size"]), "X-File-Complete": "1"},
        )
    part = device_part_dir(identity) / f"{filename}.part"
    offset = part.stat().st_size if part.exists() else 0
    return Response(status_code=200, headers={"X-Uploaded-Bytes": str(offset)})


@app.put("/coalmine/mseed/upload")
async def upload_chunk(
    request: Request,
    name: str = Query(...),
    device_id: str | None = Query(default=None),
    content_range: str | None = Header(default=None),
    x_device_id: str | None = Header(default=None),
    x_upload_token: str | None = Header(default=None),
):
    require_token(x_upload_token, UPLOAD_TOKEN, "upload")
    filename = safe_name(name)
    identity = request_device_id(device_id, x_device_id)
    match = CONTENT_RANGE_RE.fullmatch(content_range or "")
    if not match:
        raise HTTPException(status_code=400, detail="invalid Content-Range")
    start, end, total = map(int, match.groups())
    if total <= 0 or start > end or end >= total:
        raise HTTPException(status_code=400, detail="invalid byte range")

    body = await request.body()
    expected_size = end - start + 1
    if len(body) != expected_size or len(body) > MAX_CHUNK_SIZE:
        raise HTTPException(status_code=400, detail="chunk size does not match range")

    with write_lock:
        row = completed_file(identity, filename)
        if row:
            if row["size"] != total:
                raise HTTPException(status_code=409, detail="completed file has a different size")
            return Response(status_code=200, headers={"X-Uploaded-Bytes": str(total)})

        part = device_part_dir(identity) / f"{filename}.part"
        current = part.stat().st_size if part.exists() else 0
        if start != current:
            raise HTTPException(
                status_code=409,
                detail={"message": "offset mismatch", "expected_offset": current},
                headers={"X-Uploaded-Bytes": str(current)},
            )

        with part.open("ab") as stream:
            stream.write(body)
            stream.flush()
            os.fsync(stream.fileno())
        new_offset = current + len(body)

        if new_offset == total:
            digest = hashlib.sha256()
            with part.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            final = device_file_dir(identity) / filename
            os.replace(part, final)
            with database() as conn:
                ensure_device(conn, identity)
                conn.execute(
                    "INSERT INTO files(device_id, name, size, sha256) VALUES (?, ?, ?, ?)",
                    (identity, filename, total, digest.hexdigest()),
                )
                conn.execute(
                    """UPDATE devices SET last_upload_at = CURRENT_TIMESTAMP,
                       updated_at = CURRENT_TIMESTAMP WHERE device_id = ?""",
                    (identity,),
                )
            return Response(
                status_code=201,
                headers={"X-Uploaded-Bytes": str(total), "X-File-Complete": "1"},
            )

        return Response(status_code=200, headers={"X-Uploaded-Bytes": str(new_offset)})


def file_payload(request: Request, row: sqlite3.Row) -> dict:
    device_id = row["device_id"]
    file_id = row["id"]
    return {
        "id": file_id,
        "device_id": device_id,
        "name": row["name"],
        "size": row["size"],
        "sha256": row["sha256"],
        "created_at": row["created_at"],
        "download_url": str(request.base_url).rstrip("/")
        + f"/coalmine/mseed/{quote(device_id)}/files/{file_id}/{quote(row['name'])}",
    }


def find_next_file(device_id: str, after_id: int):
    with database() as conn:
        return conn.execute(
            """
            SELECT id, device_id, name, size, sha256, created_at
            FROM files WHERE device_id = ? AND id > ? ORDER BY id ASC LIMIT 1
            """,
            (device_id, after_id),
        ).fetchone()


def find_latest_file(device_id: str):
    with database() as conn:
        return conn.execute(
            """SELECT id, device_id, name, size, sha256, created_at
               FROM files WHERE device_id = ? ORDER BY id DESC LIMIT 1""",
            (device_id,),
        ).fetchone()


@app.get("/coalmine/mseed/{device_id}/next")
def next_file(
    device_id: str,
    request: Request,
    after_id: int = Query(0, ge=0),
    x_download_token: str | None = Header(default=None),
):
    require_token(x_download_token, DOWNLOAD_TOKEN, "download")
    identity = safe_device_id(device_id)
    row = find_next_file(identity, after_id)
    if not row:
        return Response(status_code=204)
    return file_payload(request, row)


@app.get("/coalmine/mseed/{device_id}/latest")
def latest_file(
    device_id: str,
    request: Request,
    x_download_token: str | None = Header(default=None),
):
    require_token(x_download_token, DOWNLOAD_TOKEN, "download")
    identity = safe_device_id(device_id)
    row = find_latest_file(identity)
    if not row:
        return Response(status_code=204)
    return file_payload(request, row)


@app.get("/coalmine/mseed/{device_id}/files/{file_id}/{requested_name}")
def download_file(
    device_id: str,
    file_id: int,
    requested_name: str,
    x_download_token: str | None = Header(default=None),
):
    require_token(x_download_token, DOWNLOAD_TOKEN, "download")
    identity = safe_device_id(device_id)
    with database() as conn:
        row = conn.execute(
            """SELECT id, device_id, name, size, sha256 FROM files
               WHERE device_id = ? AND id = ?""",
            (identity, file_id),
        ).fetchone()
    if not row or safe_name(requested_name) != row["name"]:
        raise HTTPException(status_code=404, detail="file not found")
    path = device_file_dir(identity) / row["name"]
    if not path.is_file() or path.stat().st_size != row["size"]:
        raise HTTPException(status_code=503, detail="published file is unavailable")
    return FileResponse(
        path,
        media_type="application/vnd.fdsn.mseed",
        filename=row["name"],
        headers={
            "ETag": f'"sha256:{row["sha256"]}"',
            "X-File-Id": str(row["id"]),
            "X-Device-ID": row["device_id"],
            "X-Checksum-SHA256": row["sha256"],
        },
    )


# Compatibility endpoints map old clients to device 01. New console versions
# should always use the canonical device-scoped routes above.
@app.get("/coalmine/mseed/next")
def legacy_next_file(
    request: Request,
    after_id: int = Query(0, ge=0),
    device_id: str = Query(LEGACY_DEVICE_ID),
    x_download_token: str | None = Header(default=None),
):
    return next_file(device_id, request, after_id, x_download_token)


@app.get("/coalmine/mseed/latest")
def legacy_latest_file(
    request: Request,
    device_id: str = Query(LEGACY_DEVICE_ID),
    x_download_token: str | None = Header(default=None),
):
    return latest_file(device_id, request, x_download_token)


@app.get("/coalmine/mseed/files/{file_id}/{requested_name}")
def legacy_download_file(
    file_id: int,
    requested_name: str,
    x_download_token: str | None = Header(default=None),
):
    return download_file(LEGACY_DEVICE_ID, file_id, requested_name, x_download_token)

