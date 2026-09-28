import hashlib
import os
import re
import sqlite3
import threading
from contextlib import contextmanager
from pathlib import Path
from urllib.parse import quote, unquote

from fastapi import FastAPI, Header, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse


DATA_ROOT = Path(os.environ.get("MSEED_DATA_ROOT", "/var/lib/mseed-server")).resolve()
PART_DIR = DATA_ROOT / "parts"
FILE_DIR = DATA_ROOT / "files"
DB_PATH = DATA_ROOT / "mseed.db"
UPLOAD_TOKEN = os.environ.get("MSEED_UPLOAD_TOKEN", "")
DOWNLOAD_TOKEN = os.environ.get("MSEED_DOWNLOAD_TOKEN", "")
CONTENT_RANGE_RE = re.compile(r"^bytes (\d+)-(\d+)/(\d+)$")
MAX_CHUNK_SIZE = 2 * 1024 * 1024

app = FastAPI(title="MiniCollection mseed server", version="1.0.0")
write_lock = threading.Lock()


def safe_name(raw_name: str) -> str:
    name = unquote(raw_name).strip()
    if not name or name in {".", ".."} or Path(name).name != name:
        raise HTTPException(status_code=400, detail="invalid file name")
    if not name.lower().endswith(".mseed"):
        raise HTTPException(status_code=400, detail="only .mseed files are accepted")
    return name


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
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS files (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE,
                size INTEGER NOT NULL,
                sha256 TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            )
            """
        )


@app.on_event("startup")
def startup() -> None:
    init_storage()


def completed_file(name: str):
    with database() as conn:
        return conn.execute(
            "SELECT id, name, size, sha256, created_at FROM files WHERE name = ?", (name,)
        ).fetchone()


@app.get("/healthz")
def healthz():
    return {"status": "ok"}


@app.head("/coalmine/mseed/upload")
def upload_status(
    name: str = Query(...),
    x_upload_token: str | None = Header(default=None),
):
    require_token(x_upload_token, UPLOAD_TOKEN, "upload")
    filename = safe_name(name)
    row = completed_file(filename)
    if row:
        return Response(
            status_code=200,
            headers={"X-Uploaded-Bytes": str(row["size"]), "X-File-Complete": "1"},
        )
    part = PART_DIR / f"{filename}.part"
    offset = part.stat().st_size if part.exists() else 0
    return Response(status_code=200, headers={"X-Uploaded-Bytes": str(offset)})


@app.put("/coalmine/mseed/upload")
async def upload_chunk(
    request: Request,
    name: str = Query(...),
    content_range: str | None = Header(default=None),
    x_upload_token: str | None = Header(default=None),
):
    require_token(x_upload_token, UPLOAD_TOKEN, "upload")
    filename = safe_name(name)
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
        row = completed_file(filename)
        if row:
            if row["size"] != total:
                raise HTTPException(status_code=409, detail="completed file has a different size")
            return Response(status_code=200, headers={"X-Uploaded-Bytes": str(total)})

        part = PART_DIR / f"{filename}.part"
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
            final = FILE_DIR / filename
            os.replace(part, final)
            with database() as conn:
                conn.execute(
                    "INSERT INTO files(name, size, sha256) VALUES (?, ?, ?)",
                    (filename, total, digest.hexdigest()),
                )
            return Response(
                status_code=201,
                headers={"X-Uploaded-Bytes": str(total), "X-File-Complete": "1"},
            )

        return Response(status_code=200, headers={"X-Uploaded-Bytes": str(new_offset)})


@app.get("/coalmine/mseed/next")
def next_file(
    request: Request,
    after_id: int = Query(0, ge=0),
    x_download_token: str | None = Header(default=None),
):
    require_token(x_download_token, DOWNLOAD_TOKEN, "download")
    with database() as conn:
        row = conn.execute(
            """
            SELECT id, name, size, sha256, created_at
            FROM files WHERE id > ? ORDER BY id ASC LIMIT 1
            """,
            (after_id,),
        ).fetchone()
    if not row:
        return Response(status_code=204)
    file_id = row["id"]
    return {
        "id": file_id,
        "name": row["name"],
        "size": row["size"],
        "sha256": row["sha256"],
        "created_at": row["created_at"],
        "download_url": str(request.base_url).rstrip("/")
        + f"/coalmine/mseed/files/{file_id}/{quote(row['name'])}",
    }


@app.get("/coalmine/mseed/latest")
def latest_file(
    request: Request,
    x_download_token: str | None = Header(default=None),
):
    require_token(x_download_token, DOWNLOAD_TOKEN, "download")
    with database() as conn:
        row = conn.execute(
            "SELECT id, name, size, sha256, created_at FROM files ORDER BY id DESC LIMIT 1"
        ).fetchone()
    if not row:
        return Response(status_code=204)
    return {
        "id": row["id"],
        "name": row["name"],
        "size": row["size"],
        "sha256": row["sha256"],
        "created_at": row["created_at"],
        "download_url": str(request.base_url).rstrip("/")
        + f"/coalmine/mseed/files/{row['id']}/{quote(row['name'])}",
    }


@app.get("/coalmine/mseed/files/{file_id}/{requested_name}")
def download_file(
    file_id: int,
    requested_name: str,
    x_download_token: str | None = Header(default=None),
):
    require_token(x_download_token, DOWNLOAD_TOKEN, "download")
    with database() as conn:
        row = conn.execute(
            "SELECT id, name, size, sha256 FROM files WHERE id = ?", (file_id,)
        ).fetchone()
    if not row or safe_name(requested_name) != row["name"]:
        raise HTTPException(status_code=404, detail="file not found")
    path = FILE_DIR / row["name"]
    if not path.is_file() or path.stat().st_size != row["size"]:
        raise HTTPException(status_code=503, detail="published file is unavailable")
    return FileResponse(
        path,
        media_type="application/vnd.fdsn.mseed",
        filename=row["name"],
        headers={
            "ETag": f'"sha256:{row["sha256"]}"',
            "X-File-Id": str(row["id"]),
            "X-Checksum-SHA256": row["sha256"],
        },
    )

