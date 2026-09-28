#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this installer with sudo." >&2
  exit 1
fi

SOURCE_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
APP_DIR=/opt/mseed-server
DATA_DIR=/var/lib/mseed-server
ENV_FILE=/etc/mseed-server.env
SERVICE_FILE=/etc/systemd/system/mseed-server.service
NGINX_SITE=/etc/nginx/sites-available/mseed-server

for required in mseed_server.py requirements.txt mseed-server.service nginx-mseed.conf; do
  if [[ ! -f "$SOURCE_DIR/$required" ]]; then
    echo "Missing $SOURCE_DIR/$required" >&2
    exit 1
  fi
done

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y python3 python3-venv nginx curl openssl

if ! id mseed >/dev/null 2>&1; then
  useradd --system --home-dir "$DATA_DIR" --shell /usr/sbin/nologin mseed
fi

install -d -o root -g root -m 755 "$APP_DIR"
install -d -o mseed -g mseed -m 750 "$DATA_DIR"
install -o root -g root -m 644 "$SOURCE_DIR/mseed_server.py" "$APP_DIR/mseed_server.py"
install -o root -g root -m 644 "$SOURCE_DIR/requirements.txt" "$APP_DIR/requirements.txt"

if [[ ! -x "$APP_DIR/venv/bin/python" ]]; then
  python3 -m venv "$APP_DIR/venv"
fi
"$APP_DIR/venv/bin/pip" install --upgrade pip
"$APP_DIR/venv/bin/pip" install -r "$APP_DIR/requirements.txt"

if [[ ! -f "$ENV_FILE" ]]; then
  upload_token=$(openssl rand -hex 32)
  download_token=$(openssl rand -hex 32)
  cat >"$ENV_FILE" <<EOF
MSEED_DATA_ROOT=$DATA_DIR
MSEED_UPLOAD_TOKEN=$upload_token
MSEED_DOWNLOAD_TOKEN=$download_token
EOF
  chmod 600 "$ENV_FILE"
fi

install -o root -g root -m 644 "$SOURCE_DIR/mseed-server.service" "$SERVICE_FILE"
install -o root -g root -m 644 "$SOURCE_DIR/nginx-mseed.conf" "$NGINX_SITE"
ln -sfn "$NGINX_SITE" /etc/nginx/sites-enabled/mseed-server
if [[ -L /etc/nginx/sites-enabled/default ]]; then
  rm /etc/nginx/sites-enabled/default
fi

nginx -t
systemctl daemon-reload
systemctl enable --now mseed-server.service
systemctl enable --now nginx.service
systemctl restart mseed-server.service
systemctl reload nginx.service

for attempt in {1..20}; do
  if curl --fail --silent http://127.0.0.1:9001/healthz >/dev/null; then
    break
  fi
  if [[ $attempt -eq 20 ]]; then
    echo "Health check failed. Inspect: journalctl -u mseed-server.service" >&2
    exit 1
  fi
  sleep 1
done

if command -v ufw >/dev/null 2>&1 && ufw status | grep -q '^Status: active'; then
  ufw allow OpenSSH
  ufw allow 'Nginx HTTP'
fi

server_ip=$(hostname -I | awk '{print $1}')
echo
echo "mseed server installation completed."
echo "Health URL: http://${server_ip}/healthz"
echo "Service status: systemctl status mseed-server.service"
echo "Credentials (keep this file private):"
cat "$ENV_FILE"
