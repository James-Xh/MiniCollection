#!/usr/bin/env bash
# Clear one project's test data on the server: uploaded mseed files, partial
# uploads, file rows, and optionally the whole project database.
#
#   sudo ./reset-server.sh [project-name] [--reset-db] [--yes] [--dry-run]
#
# Default project: project_lgy.
# The service is stopped before deleting and started again afterwards.
set -euo pipefail

PROJECT=project_lgy
if [[ $# -gt 0 && "$1" != -* ]]; then
  PROJECT=$1
  shift
fi

RESET_DB=false
ASSUME_YES=false
DRY_RUN=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --reset-db) RESET_DB=true ;;
    --yes|-y) ASSUME_YES=true ;;
    --dry-run) DRY_RUN=true ;;
    -h|--help)
      echo "Usage: sudo $0 [project-name] [--reset-db] [--yes] [--dry-run]"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
  shift
done

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi
if [[ ! "$PROJECT" =~ ^[a-z0-9][a-z0-9_-]*$ ]]; then
  echo "Invalid project name: $PROJECT" >&2
  exit 1
fi

ENV_FILE=/etc/mseed-server-$PROJECT.env
SERVICE=mseed-server-$PROJECT.service
if [[ ! -f "$ENV_FILE" ]]; then
  echo "Missing $ENV_FILE" >&2
  exit 1
fi

DATA_ROOT=$(grep -E '^MSEED_DATA_ROOT=' "$ENV_FILE" | head -1 | cut -d= -f2-)
if [[ -z "$DATA_ROOT" || "$DATA_ROOT" == "/" ]]; then
  echo "Refusing to delete: MSEED_DATA_ROOT is empty or /" >&2
  exit 1
fi
if [[ ! -d "$DATA_ROOT" ]]; then
  echo "Data root does not exist: $DATA_ROOT" >&2
  exit 1
fi

DB=$DATA_ROOT/mseed.db
FILES_DIR=$DATA_ROOT/files
PARTS_DIR=$DATA_ROOT/parts

file_count=0
part_count=0
if [[ -d "$FILES_DIR" ]]; then
  file_count=$(find "$FILES_DIR" -type f | wc -l)
fi
if [[ -d "$PARTS_DIR" ]]; then
  part_count=$(find "$PARTS_DIR" -type f | wc -l)
fi

echo "Project:      $PROJECT"
echo "Service:      $SERVICE"
echo "Data root:    $DATA_ROOT"
echo "Complete files: $file_count"
echo "Partial files:  $part_count"
echo "Database:     $DB"
if $RESET_DB; then
  echo "Mode:         reset database (device configs/history will be lost)"
else
  echo "Mode:         keep database, delete file rows only"
fi
echo

if $DRY_RUN; then
  echo "Dry run only. Nothing was deleted."
  [[ -d "$FILES_DIR" ]] && du -sh "$FILES_DIR" || true
  [[ -d "$PARTS_DIR" ]] && du -sh "$PARTS_DIR" || true
  exit 0
fi

if ! $ASSUME_YES; then
  read -r -p "Type RESET to delete these test files: " answer
  [[ "$answer" == "RESET" ]] || { echo "Cancelled."; exit 1; }
fi

service_was_active=false
if systemctl is-active --quiet "$SERVICE"; then
  service_was_active=true
  systemctl stop "$SERVICE"
fi

if [[ -f "$DB" ]]; then
  backup=/root/mseed-$PROJECT-db-$(date +%Y%m%d-%H%M%S).bak
  cp -a "$DB" "$backup"
  echo "Database backup: $backup"
fi

PYTHON=/opt/mseed-server/venv/bin/python
[[ -x "$PYTHON" ]] || PYTHON=python3

if $RESET_DB; then
  rm -f "$DB" "$DB-wal" "$DB-shm"
else
  if [[ -f "$DB" ]]; then
    "$PYTHON" - "$DB" <<'PY'
import sqlite3
import sys

conn = sqlite3.connect(sys.argv[1])
try:
    conn.execute("DELETE FROM files")
    conn.commit()
    conn.execute("VACUUM")
    conn.commit()
    print("Deleted rows from files table.")
except sqlite3.OperationalError as exc:
    print(f"Database cleanup skipped: {exc}")
finally:
    conn.close()
PY
  fi
fi

rm -rf "$FILES_DIR" "$PARTS_DIR"

if $service_was_active; then
  systemctl start "$SERVICE"
  systemctl is-active --quiet "$SERVICE" || {
    echo "Service failed to start: $SERVICE" >&2
    exit 1
  }
fi

echo
echo "Server test data cleared for project $PROJECT."
echo "Complete files: $file_count, partial files: $part_count deleted."
if $RESET_DB; then
  echo "Database was reset; device configs and applied versions will be recreated."
fi
echo "Next: run the console reset script so its cursor is cleared too."
