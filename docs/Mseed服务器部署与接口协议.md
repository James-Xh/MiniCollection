# mseed 服务器部署与接口协议

## 目标

- MiniCollection 自动扫描 `data/picker/*.mseed`，按设备编号分片上传；
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
HEAD /coalmine/mseed/upload?device_id=01&name=文件名
PUT  /coalmine/mseed/upload?device_id=01&name=文件名
Content-Range: bytes start-end/total
X-Device-ID: 01
```

配置文件 `/opt/MiniCollection/data/Config.ini`：

```ini
[Upload]
enabled=1
serverUrl=https://你的域名
token=与服务器MSEED_UPLOAD_TOKEN相同的上传令牌
pollSec=5
```

设备编号由编译参数 `MINICOLLECTION_DEVICE_ID` 固定，客户端在 HEAD 和 PUT 请求中同时发送
`device_id` 查询参数和 `X-Device-ID` 请求头。客户端也会发送 `X-Upload-Token`。修改配置后重启
`minicollection.service` 使 token 生效。若将服务器 token 留空会关闭认证，只适用于隔离的可信内网。

上传状态保存在目标机：

```text
/opt/MiniCollection/data/upload/01/progress.ini
/opt/MiniCollection/data/upload/01/done.txt
```

服务端以 `X-Uploaded-Bytes` 为权威偏移，网络恢复后会从服务器实际长度继续上传。

## 六、上传协议行为

1. HEAD 返回 `.part` 文件已写入字节数；完整文件返回总大小。
2. PUT 必须从服务器当前偏移开始，否则返回 HTTP 409 和正确偏移。
3. 每个分片写入后执行 `fsync`。
4. 最后一片完成后计算 SHA-256。
5. 使用 `os.replace()` 从 `parts/设备编号/*.part` 原子移动到
   `files/设备编号/`。
6. 原子移动完成后才写入 SQLite，因此下载接口不会看到半文件。
7. 文件名为上传幂等键；同名不同大小返回 409，避免静默覆盖。

## 七、消费端获取文件

不要定时调用 `latest` 后仅按文件名去重。正确做法是保存服务端单调递增的 `id` 游标。

第一次请求：

```bash
curl -H 'X-Download-Token: 下载令牌' \
  'https://你的域名/coalmine/mseed/01/next?after_id=0'
```

有文件时返回：

```json
{
  "id": 101,
  "device_id": "01",
  "name": "example.mseed",
  "size": 123456,
  "sha256": "...",
  "created_at": "2026-09-28 10:00:00",
  "download_url": "https://你的域名/coalmine/mseed/01/files/101/example.mseed"
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

`GET /coalmine/mseed/01/latest` 只适合查看当前最新文件，不适合可靠消费，因为轮询间隔内产生多个
文件时可能跳过中间文件。

## 八、运维检查

```bash
sudo systemctl status mseed-server.service
sudo journalctl -u mseed-server.service -n 100 --no-pager
sudo du -sh /var/lib/mseed-server
sudo sqlite3 /var/lib/mseed-server/mseed.db \
  'select id,device_id,name,size,sha256,created_at from files order by id desc limit 10;'
```

备份时同时备份：

```text
/var/lib/mseed-server/files/设备编号/
/var/lib/mseed-server/mseed.db
```

不要把 `parts/` 中的 `.part` 文件提供给下载端。可以定期清理长期没有更新且确认客户端不再续传
的 `.part`，但不能在上传期间删除。

## 九、从旧版服务器升级

升级前先备份数据库和文件目录，然后替换服务程序：

```bash
sudo systemctl stop mseed-server.service
sudo cp -a /var/lib/mseed-server /var/lib/mseed-server.backup-$(date +%Y%m%d-%H%M%S)
sudo install -o root -g root -m 644 mseed_server.py /opt/mseed-server/mseed_server.py
sudo install -o root -g root -m 644 nginx-mseed.conf /etc/nginx/sites-available/mseed-server
sudo nginx -t
sudo systemctl start mseed-server.service
sudo systemctl reload nginx.service
sudo systemctl --no-pager --full status mseed-server.service
```

首次启动会自动为 `files` 表增加 `device_id`，将旧记录归入设备 `01`，并将旧的平铺文件和
未完成分片分别移动到 `files/01/`、`parts/01/`。迁移后检查：

```bash
sudo find /var/lib/mseed-server/files -maxdepth 2 -type f | head
sudo sqlite3 /var/lib/mseed-server/mseed.db \
  'select device_id,count(*) from files group by device_id;'
curl -H 'X-Download-Token: 下载令牌' \
  'http://127.0.0.1:9001/coalmine/mseed/01/next?after_id=0'
```

旧的 `/coalmine/mseed/next`、`latest`、`files` 下载接口仍兼容设备 `01`。新控制台应使用包含
设备编号的规范接口，且每台设备分别保存自己的消费游标。

## 十、设备配置和运行状态接口

控制台创建设备：

```http
PUT /coalmine/devices/02
Content-Type: application/json

{"name":"EG628-02"}
```

控制台提交完整配置。服务器为每台设备独立生成递增版本号：

```http
PUT /coalmine/devices/02/config
Content-Type: application/json

{
  "parameters": {
    "tri_on": 5.5, "tri_off": 2,
    "nsta": 50, "nlta": 500,
    "detect_ch": 1, "qualified_ch": 1,
    "energy_thre": 0.25, "amp_thre": 0.25,
    "intrach": 100, "interch": 250
  },
  "upload_server_url": "http://39.96.41.239:443"
}
```

EG628 每隔固定时间请求新版本；版本没有变化时返回 HTTP 204：

```http
GET /coalmine/devices/02/config/latest?after_version=3
```

EG628 在算法线程真正应用完整参数快照后回报结果：

```http
POST /coalmine/devices/02/config/applied
Content-Type: application/json

{"version":4,"success":true,"message":"runtime applied"}
```

EG628 定期提交运行状态：

```http
POST /coalmine/devices/02/heartbeat
Content-Type: application/json

{
  "program_running": true,
  "program_version": "1.0.0",
  "network_state": "online",
  "local_ip": "192.168.111.2",
  "applied_config_version": 4,
  "stations": [{
    "station_number": 1,
    "ip": "192.168.111.10",
    "port": 5000,
    "connected": true,
    "online_channels": 12,
    "total_channels": 16,
    "gps": 5
  }]
}
```

控制台使用以下接口展示状态（均需 `X-Project-Token`，见第十一节）：

```text
GET    /coalmine/devices
GET    /coalmine/devices/02
GET    /coalmine/devices/02/config/status
DELETE /coalmine/devices/02
```

设备超过 30 秒没有心跳时，查询结果中的 `online` 为 `false`。删除设备采用停用方式，历史配置和
mseed 文件不会被删除；设备再次发送心跳时会恢复启用。

控制面的所有 `/coalmine/devices...` 接口都需要请求头 `X-Project-Token`，值为本项目的项目令牌
（控制台统一使用下载令牌作为项目令牌）。令牌不匹配返回 HTTP 401，
不会列出、修改或返回其它项目的设备。

## 十一、多项目（多控制台）隔离部署

### 11.1 隔离模型

一个项目 = 一个独立服务进程 + 一个独立数据目录 + 一组独立令牌。不要让多个项目共用一个
`MSEED_DATA_ROOT`，也不要让多个项目共用同一个服务进程。

```text
控制台 A ──下载/项目令牌 A──┐
设备 A1/A2 ──上传令牌 A─────┼─> mseed-server-project_a :10001 ─> /var/lib/mseed-server-projects/project_a
                            │
控制台 B ──下载/项目令牌 B──┐
设备 B1/B2 ──上传令牌 B─────┼─> mseed-server-project_b :10002 ─> /var/lib/mseed-server-projects/project_b
```

每个项目服务只认识自己的令牌和数据库，因此：

- 控制台的设备列表、心跳、参数下发、配置状态只覆盖本项目设备；
- 设备把上传令牌当作自己的项目身份；指向错误项目的服务地址时，服务端返回 401；
- 不同项目可以使用相同的设备编号（例如都有 `01`），数据仍分别存放在各自目录；
- 删除或停用一个项目服务不影响其它项目的运行。

### 11.2 令牌角色

| 环境变量 | 使用者 | 用途 | 控制面是否接受 |
| --- | --- | --- | --- |
| `MSEED_UPLOAD_TOKEN` | 设备 | mseed 分片上传 | 是（设备的心跳/参数请求使用它） |
| `MSEED_DOWNLOAD_TOKEN` | 控制台 | mseed 下载、消费 | 是（控制台默认用它作项目令牌） |
| `MSEED_PROJECT_TOKEN` | 自定义控制端（可选） | 专用控制面令牌；控制台不使用 | 是 |
| `MSEED_PROJECT_NAME` | 服务/控制台展示 | 项目名称 | - |
| `MSEED_PUBLIC_BASE_URL` | 服务 | 生成下载链接的公网地址 | - |

上传令牌、下载令牌、控制令牌是三类不同权限，不能互相替代上传或下载接口。三者都是每个项目
独立的随机值；不要把同一个令牌复制给多个项目，否则该项目之间就不再隔离。生产环境不要留空，
留空会关闭对应认证，只适用于隔离的测试内网。

### 11.3 新增一个项目

在服务器上，把整个 `server/` 目录上传到 `/tmp/mseed-server-deploy`，先完成单项目安装（见本文
第一至四节），然后执行：

```bash
cd /tmp/mseed-server-deploy
chmod +x add-project.sh
sudo ./add-project.sh project_lgy 9002
```

参数含义：

```text
add-project.sh <项目名> <公网端口> [内部端口] [公网基地址]
```

- 项目名只允许小写字母、数字、`-`、`_`；
- 默认内部端口 = 公网端口 + 1000，只监听 `127.0.0.1`；
- 公网基地址可选，用于 HTTPS 域名，例如 `https://lgy.example.com`；不填则输出
  `http://服务器IP:公网端口`。

脚本会创建：

```text
/opt/mseed-server-projects/project_lgy/mseed_server.py         程序副本
/var/lib/mseed-server-projects/project_lgy/                    独立数据目录/SQLite
/etc/mseed-server-project_lgy.env                              root-only 令牌
/etc/systemd/system/mseed-server-project_lgy.service           独立服务
/etc/nginx/sites-available/mseed-server-project_lgy            独立 Nginx 站点
```

并自动启动服务、重载 Nginx、允许防火墙端口。脚本可重复执行：已有令牌和数据不会被覆盖，
程序、服务和 Nginx 配置会刷新。

输出示例：

```text
Project project_lgy is ready.
  Public URL:      http://203.0.113.10:9002
  Internal port:   127.0.0.1:10002
  Data root:       /var/lib/mseed-server-projects/project_lgy (per-project database)

Device /opt/MiniCollection/data/Config.ini:
  [Upload]
  serverUrl=http://203.0.113.10:9002
  token=<上传令牌>

Console config.ini:
  [Server]
  url=http://203.0.113.10:9002
  downloadToken=<下载令牌>
```

再新增项目 B 只需换项目名和端口：

```bash
sudo ./add-project.sh project_b 9003
```

### 11.4 设备端配置

每台设备只填写自己项目的地址和上传令牌：

```ini
[Upload]
enabled=1
serverUrl=http://203.0.113.10:9002
token=project_lgy的上传令牌
```

修改后重启 `minicollection.service`。设备的参数拉取、心跳和应用回执都会带
`X-Project-Token`（值为上传令牌），服务端据此确认它属于本项目；令牌或地址填错时请求返回
401，不会被其它项目接管。

### 11.5 控制台配置

```ini
[Server]
url=http://203.0.113.10:9002
downloadToken=project_lgy的下载令牌

[Consumer]
pollSeconds=5
outputDir=data/mseed
stateFile=data/state
```

- `downloadToken` 用于下载 mseed，同时作为控制面令牌；
- 控制台不再读取 `projectToken`，旧配置里的该键会在下一次保存时删除；
- `MSEED_PROJECT_TOKEN` 仅供自定义控制端使用，控制台不使用；
- 控制台标题会显示 `/coalmine/project` 返回的项目名，便于现场确认连接的是哪个项目；
- 每个控制台只填写自己项目的地址和下载令牌。不同控制台可以安装在同一台电脑上，使用不同
  `config.ini` 或不同输出目录。

### 11.6 验证隔离

```bash
# 项目自己的令牌：200
curl -H 'X-Project-Token: 项目A下载令牌' http://127.0.0.1:10001/coalmine/devices

# 不带令牌或使用项目B的令牌：401
curl -i http://127.0.0.1:10001/coalmine/devices
curl -i -H 'X-Project-Token: 项目B下载令牌' http://127.0.0.1:10001/coalmine/devices

# 确认服务对应的项目
curl -H 'X-Project-Token: 项目A下载令牌' http://127.0.0.1:10001/coalmine/project
```

设备实际归属也可以通过心跳后的设备列表确认：

```bash
curl -H 'X-Project-Token: 项目A下载令牌' http://127.0.0.1:10001/coalmine/devices
```

项目 A 的列表里只会出现携带项目 A 令牌发送心跳的设备。

### 11.7 设备换项目

设备换项目必须同时修改设备配置中的 `serverUrl` 和 `token`，然后重启采集程序：

1. 在新项目服务中新增/等待设备心跳；
2. 修改设备 `/opt/MiniCollection/data/Config.ini`；
3. 重启 `minicollection.service`；
4. 旧项目的设备会在 30 秒后显示离线，历史 mseed 仍留在旧项目数据目录。

只改 `serverUrl` 不改令牌不会迁移成功，新项目会拒绝该设备的心跳和配置请求，这是刻意的
防止误接管行为。需要保留旧手持上传进度时，同时迁移设备本地
`data/upload/<设备编号>/`（可选，服务端会按实际偏移续传）。

设备换项目时还要注意配置版本号。新版设备固件不再把 `appliedConfigVersion` 写入
`Config.ini`，而是每次轮询都拉取服务端最新配置，只用进程内存中的版本避免重复应用；因此换项目后
第一次心跳就会应用新项目的最新配置，点击下发也会立即生效。旧版固件仍以本地
`[Remote] appliedConfigVersion` 作为高水位，如果旧项目的已应用版本大于新项目当前配置版本，
会一直不拉取。兼容处理有两种：

- 升级服务端：新配置版本使用 `max(项目历史最大版本, 设备已应用版本) + 1`，重新下发即可；
- 或停设备，把 `[Remote] appliedConfigVersion` 改为 `0`，再启动设备。

### 11.8 HTTPS、子域名和反向代理

脚本默认生成 HTTP 端口站点，适合内网或测试。生产环境建议每个项目使用独立子域名：

```nginx
server {
    listen 443 ssl;
    server_name lgy.example.com;

    ssl_certificate     /etc/letsencrypt/live/lgy.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/lgy.example.com/privkey.pem;

    client_max_body_size 2m;

    location / {
        proxy_pass http://127.0.0.1:10002;
        proxy_http_version 1.1;
        proxy_set_header Host $http_host;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_request_buffering off;
        proxy_read_timeout 60s;
    }
}
```

同时在项目 env 中设置：

```ini
MSEED_PUBLIC_BASE_URL=https://lgy.example.com
```

然后 `sudo systemctl restart mseed-server-project_lgy.service`。否则服务端生成的
`download_url` 可能仍是内部 HTTP 地址。设备和控制台的 `serverUrl` 填
`https://lgy.example.com`。

不要用同一个 `server_name` + 同一个端口同时承载两个项目；也不要用路径前缀复用同一个
项目服务。项目隔离依赖独立端口/子域名和独立服务进程。

### 11.9 运维

```bash
sudo systemctl status mseed-server-project_lgy.service
sudo journalctl -u mseed-server-project_lgy.service -n 100 --no-pager
sudo systemctl restart mseed-server-project_lgy.service
sudo nginx -t && sudo systemctl reload nginx
```

备份时按项目分别备份：

```text
/var/lib/mseed-server-projects/<项目名>/files/
/var/lib/mseed-server-projects/<项目名>/mseed.db
/etc/mseed-server-<项目名>.env
```

服务端仍使用单 worker（分片写入锁为进程内锁）。多项目通过多进程隔离，不要给单个项目增加
`--workers`。
