#!/usr/bin/env bash
# Clear the acquisition device's local test data:
#   - upload resume/done state (data/upload/<device-id>/)
#   - event mseed files (data/picker, data/temp, data/invalid)
#   - optionally continuous raw data (data/bins)
#
#   sudo ./reset-device.sh [--app-dir /opt/MiniCollection] [--include-bins] [--yes] [--dry-run]
set -euo pipefail

APP_DIR=/opt/MiniCollection
INCLUDE_BINS=false
ASSUME_YES=false
DRY_RUN=false
while [[ $# -gt 0 ]]; do
  case "$1" in
    --app-dir)
      [[ $# -ge 2 ]] || { echo "--app-dir needs a value" >&2; exit 1; }
      APP_DIR=$2
      shift 2
      ;;
    --include-bins) INCLUDE_BINS=true; shift ;;
    --yes|-y) ASSUME_YES=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    -h|--help)
      echo "Usage: sudo $0 [--app-dir /opt/MiniCollection] [--include-bins] [--yes] [--dry-run]"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
  esac
done

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi
if [[ ! -d "$APP_DIR/data" ]]; then
  echo "Data directory not found: $APP_DIR/data" >&2
  exit 1
fi

SERVICE=minicollection.service
DATA=$APP_DIR/data
UPLOAD=$DATA/upload
PICKER=$DATA/picker
TEMP=$DATA/temp
INVALID=$DATA/invalid
BINS=$DATA/bins

count_files() {
  local dir=$1
  [[ -d "$dir" ]] || { echo 0; return; }
  find "$dir" -type f | wc -l
}

count_mseed() {
  local dir=$1
  [[ -d "$dir" ]] || { echo 0; return; }
  find "$dir" -maxdepth 1 -type f -name '*.mseed' | wc -l
}

echo "App dir:   $APP_DIR"
echo "Upload state files: $(count_files "$UPLOAD")"
echo "picker mseed:       $(count_mseed "$PICKER")"
echo "temp mseed:         $(count_mseed "$TEMP")"
echo "invalid mseed:      $(count_mseed "$INVALID")"
if $INCLUDE_BINS; then
  echo "bins contents:      $(count_files "$BINS") (will be deleted)"
else
  echo "bins contents:      $(count_files "$BINS") (kept; use --include-bins to delete)"
fi
echo

if $DRY_RUN; then
  echo "Dry run only. Nothing was deleted."
  [[ -d "$UPLOAD" ]] && du -sh "$UPLOAD" || true
  [[ -d "$PICKER" ]] && du -sh "$PICKER" || true
  [[ -d "$TEMP" ]] && du -sh "$TEMP" || true
  [[ -d "$INVALID" ]] && du -sh "$INVALID" || true
  exit 0
fi

if ! $ASSUME_YES; then
  read -r -p "Type RESET to delete device test data: " answer
  [[ "$answer" == "RESET" ]] || { echo "Cancelled."; exit 1; }
fi

service_was_active=false
if systemctl is-active --quiet "$SERVICE"; then
  service_was_active=true
  systemctl stop "$SERVICE"
else
  if pgrep -x MiniCollection >/dev/null 2>&1 || pgrep -x MiniWatchdog >/dev/null 2>&1; then
    echo "MiniCollection is running outside $SERVICE. Stop it first." >&2
    exit 1
  fi
fi

if [[ -d "$UPLOAD" ]]; then
  find "$UPLOAD" -mindepth 1 -delete
fi

for dir in "$PICKER" "$TEMP" "$INVALID"; do
  if [[ -d "$dir" ]]; then
    find "$dir" -maxdepth 1 -type f -name '*.mseed' -delete
  fi
done

if $INCLUDE_BINS && [[ -d "$BINS" ]]; then
  find "$BINS" -mindepth 1 -delete
fi

if $service_was_active; then
  systemctl start "$SERVICE"
  systemctl is-active --quiet "$SERVICE" || {
    echo "Service failed to start: $SERVICE" >&2
    exit 1
  }
fi

echo
echo "Device test data cleared."
echo "Upload progress/done state, picker/temp/invalid mseed deleted."
if $INCLUDE_BINS; then
  echo "data/bins contents deleted."
else
  echo "data/bins kept."
fi
