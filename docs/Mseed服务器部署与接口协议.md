# mseed 服务器部署与接口协议

## 目标

- MiniCollection 自动扫描 `data/picker/*.mseed` 并分片上传；
- 网络中断或进程重启后从服务端实际偏移继续；
- 只有完整文件会对下载端可见；
- 消费程序使用数据库 `id` 游标顺序获取，不重复、不漏取；
- 下载后通过文件长度和 SHA-256 校验完整性。

服务端实现位于 `server/mseed_server.py`，兼容现有客户端的 `HEAD + PUT Content-Range`
协议。服务器需要开放 HTTP/HTTPS 端口，9001 仅监听本机，由 Nginx 反向代理。

## 一、服务器准备

以下示例适用于 Ubuntu 22.04/24.04 服务器。执行：

### 推荐：安装脚本

在本地项目根目录，将整个服务目录上传到服务器（替换用户名和服务器 IP）：

```bash
scp -r server user@服务器IP:/tmp/mseed-server-deploy
```

SSH 登录服务器：

```bash
ssh user@服务器IP
```

在服务器执行：

```bash
cd /tmp/mseed-server-deploy
chmod +x install-ubuntu-22.04.sh
sudo ./install-ubuntu-22.04.sh
```

脚本会安装依赖、生成令牌、安装 Nginx、创建并启用 systemd 服务。安装完成后保存脚本输出的
`MSEED_UPLOAD_TOKEN` 和 `MSEED_DOWNLOAD_TOKEN`。令牌也保存在服务器的
`/etc/mseed-server.env`，权限为 root-only。

验证开机自启和接口：

```bash
sudo systemctl is-enabled mseed-server.service nginx.service
sudo systemctl is-active mseed-server.service nginx.service
curl http://127.0.0.1/healthz
```

预期两个服务都是 `enabled`、`active`，健康接口返回 `{"status":"ok"}`。

以下为安装脚本内部步骤，手工部署时使用：

```bash
sudo apt update
sudo apt install -y python3 python3-venv nginx
sudo useradd --system --home /var/lib/mseed-server --shell /usr/sbin/nologin mseed || true
sudo mkdir -p /opt/mseed-server /var/lib/mseed-server
sudo chown -R mseed:mseed /var/lib/mseed-server
```

从项目目录上传服务端文件：

```bash
scp server/mseed_server.py server/requirements.txt \
    server/mseed-server.service server/nginx-mseed.conf \
    user@服务器IP:/tmp/
```

服务器执行：

```bash
sudo install -o root -g root -m 644 /tmp/mseed_server.py /opt/mseed-server/
sudo install -o root -g root -m 644 /tmp/requirements.txt /opt/mseed-server/
sudo python3 -m venv /opt/mseed-server/venv
sudo /opt/mseed-server/venv/bin/pip install -r /opt/mseed-server/requirements.txt
```

## 二、认证和存储配置

生成两个独立随机令牌：

```bash
openssl rand -hex 32
openssl rand -hex 32
```

创建 `/etc/mseed-server.env`：

```bash
sudo tee /etc/mseed-server.env >/dev/null <<'EOF'
MSEED_DATA_ROOT=/var/lib/mseed-server
MSEED_UPLOAD_TOKEN=替换为上传令牌
MSEED_DOWNLOAD_TOKEN=替换为下载令牌
EOF
sudo chmod 600 /etc/mseed-server.env
```

上传令牌只交给 EG628；下载令牌只交给消费程序。生产环境不要留空。

## 三、安装 systemd 服务

```bash
sudo install -o root -g root -m 644 /tmp/mseed-server.service \
  /etc/systemd/system/mseed-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now mseed-server.service
sudo systemctl --no-pager --full status mseed-server.service
curl http://127.0.0.1:9001/healthz
```

服务使用单 worker，因为分片文件写入使用进程内锁。需要横向扩容时应改用对象存储或数据库锁，
不能简单增加 `--workers`。

## 四、配置 Nginx

```bash
sudo install -o root -g root -m 644 /tmp/nginx-mseed.conf \
  /etc/nginx/sites-available/mseed-server
sudo ln -s /etc/nginx/sites-available/mseed-server \
  /etc/nginx/sites-enabled/mseed-server
sudo nginx -t
sudo systemctl reload nginx
curl http://服务器IP/healthz
```

公网服务器必须配置域名、HTTPS 证书和防火墙。客户端 `serverUrl` 应使用
`https://你的域名`，不要长期使用明文 HTTP 传输令牌和数据。

## 五、EG628 上传配置

当前客户端已支持：

```text
HEAD /coalmine/mseed/upload?name=文件名
PUT  /coalmine/mseed/upload?name=文件名
Content-Range: bytes start-end/total
```

配置文件 `/opt/MiniCollection/data/Config.ini`：

```ini
[Upload]
enabled=1
serverUrl=https://你的域名
token=与服务器MSEED_UPLOAD_TOKEN相同的上传令牌
pollSec=5
```

客户端会在 HEAD 和 PUT 请求中发送 `X-Upload-Token`。修改配置后重启
`minicollection.service` 使 token 生效。若将服务器 token 留空会关闭认证，只适用于隔离的可信内网。

上传状态保存在目标机：

```text
/opt/MiniCollection/data/upload/progress.ini
/opt/MiniCollection/data/upload/done.txt
```

服务端以 `X-Uploaded-Bytes` 为权威偏移，网络恢复后会从服务器实际长度继续上传。

## 六、上传协议行为

1. HEAD 返回 `.part` 文件已写入字节数；完整文件返回总大小。
2. PUT 必须从服务器当前偏移开始，否则返回 HTTP 409 和正确偏移。
3. 每个分片写入后执行 `fsync`。
4. 最后一片完成后计算 SHA-256。
5. 使用 `os.replace()` 从 `parts/*.part` 原子移动到 `files/`。
6. 原子移动完成后才写入 SQLite，因此下载接口不会看到半文件。
7. 文件名为上传幂等键；同名不同大小返回 409，避免静默覆盖。

## 七、消费端获取文件

不要定时调用 `latest` 后仅按文件名去重。正确做法是保存服务端单调递增的 `id` 游标。

第一次请求：

```bash
curl -H 'X-Download-Token: 下载令牌' \
  'https://你的域名/coalmine/mseed/next?after_id=0'
```

有文件时返回：

```json
{
  "id": 101,
  "name": "example.mseed",
  "size": 123456,
  "sha256": "...",
  "created_at": "2026-09-28 10:00:00",
  "download_url": "https://你的域名/coalmine/mseed/files/101/example.mseed"
}
```

没有新文件时返回 HTTP `204 No Content`。

下载：

```bash
curl --fail --location \
  -H 'X-Download-Token: 下载令牌' \
  -o example.mseed.part \
  '返回的 download_url'
```

消费端必须按以下顺序处理：

1. 请求 `next?after_id=<本地已确认ID>`；
2. 下载到临时文件 `name.mseed.part`；
3. 校验实际大小等于 JSON 中 `size`；
4. 校验 SHA-256 等于 JSON 中 `sha256`；
5. 原子重命名为 `name.mseed`；
6. 最后持久化新的 `id` 游标；
7. 继续请求 `next?after_id=<新ID>`，直到收到 204。

只有第 6 步成功后才推进游标。若程序在之前崩溃，会重新下载同一 ID，但最终文件处理应以 ID
幂等；这样不会丢文件。业务处理完成才推进游标，可实现“至少一次传递”；业务侧用 `id` 唯一键
可达到效果上的不重复处理。

`GET /coalmine/mseed/latest` 只适合查看当前最新文件，不适合可靠消费，因为轮询间隔内产生多个
文件时可能跳过中间文件。

## 八、运维检查

```bash
sudo systemctl status mseed-server.service
sudo journalctl -u mseed-server.service -n 100 --no-pager
sudo du -sh /var/lib/mseed-server
sudo sqlite3 /var/lib/mseed-server/mseed.db \
  'select id,name,size,sha256,created_at from files order by id desc limit 10;'
```

备份时同时备份：

```text
/var/lib/mseed-server/files/
/var/lib/mseed-server/mseed.db
```

不要把 `parts/` 中的 `.part` 文件提供给下载端。可以定期清理长期没有更新且确认客户端不再续传
的 `.part`，但不能在上传期间删除。
