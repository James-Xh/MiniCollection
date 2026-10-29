# MiniCollection EG628 编译、部署与开机启动操作手册

本文适用于以下已验证环境：

- 编译机：Ubuntu 22.04 x86_64
- 目标机：EG628，AArch64，glibc 2.31
- 交叉编译器：Linaro GCC 10.2.1 `aarch64-linux-gnu-*`
- 目标 Qt：Qt 5.12.8
- 编译目录：`~/work/MiniCollection/build-eg628-qt6`
- 目标部署目录：`/opt/MiniCollection`
- 目标机地址：`192.168.111.2`

## 一、环境路径

每次新开编译机终端后执行：

```bash
export EG628_TC="$HOME/gcc-linaro-10.2.1-2021.01-x86_64_aarch64-linux-gnu"
export TARGET_SYSROOT="$HOME/eg628-sysroot"
export Qt5_DIR="$TARGET_SYSROOT/usr/lib/aarch64-linux-gnu/cmake/Qt5"
export PATH="$EG628_TC/bin:$PATH"
```

检查环境：

```bash
aarch64-linux-gnu-g++ -dumpmachine
test -f "$Qt5_DIR/Qt5Config.cmake" && echo "Qt5 OK"
"$HOME/bin/moc-eg628" -v
```

预期输出包含：

```text
aarch64-linux-gnu
Qt5 OK
moc 5.12.8
```

可将上述 `export` 命令写入编译机的 `~/.bashrc`，以后只需执行：

```bash
source "$HOME/.bashrc"
```

## 二、修改代码后的日常编译

进入项目：

```bash
cd "$HOME/work/MiniCollection"
```

通常只需执行增量编译：

```bash
cmake --build build-eg628-qt6 --parallel "$(nproc)"
```

Ninja 只重新编译发生变化的文件。

如果修改了 `CMakeLists.txt`、工具链文件、Qt 路径或新增了源文件，先重新配置：

```bash
cmake -S . -B build-eg628-qt6 \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/eg628-aarch64.cmake" \
  -DQt5_DIR="$Qt5_DIR" \
  -DCMAKE_PREFIX_PATH="$TARGET_SYSROOT/usr" \
  -DQT_HOST_MOC="$HOME/bin/moc-eg628" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-eg628-qt6 --parallel "$(nproc)"
```

构建成功后生成：

```text
build-eg628-qt6/MiniCollection
build-eg628-qt6/MiniWatchdog
```

## 三、部署前检查

检查程序架构：

```bash
file build-eg628-qt6/MiniCollection \
     build-eg628-qt6/MiniWatchdog
```

两个文件都应包含：

```text
ELF 64-bit ... ARM aarch64
```

检查 glibc 需求：

```bash
for app in MiniCollection MiniWatchdog; do
  echo "===== $app ====="
  aarch64-linux-gnu-readelf --version-info "build-eg628-qt6/$app" \
    | grep -o 'GLIBC_[0-9.]*' \
    | sort -Vu \
    | tail -n 1
done
```

最高版本不得超过目标机的 `GLIBC_2.31`。

## 四、首次部署

在编译机执行：

```bash
ssh root@192.168.111.2 'mkdir -p /opt/MiniCollection'

scp build-eg628-qt6/MiniCollection \
    build-eg628-qt6/MiniWatchdog \
    root@192.168.111.2:/opt/MiniCollection/
```

在目标机设置权限并检查依赖：

```bash
chmod 755 /opt/MiniCollection/MiniCollection
chmod 755 /opt/MiniCollection/MiniWatchdog

ldd /opt/MiniCollection/MiniCollection
ldd /opt/MiniCollection/MiniWatchdog
```

输出中不能出现 `not found`。

首次运行会在程序目录创建或使用：

```text
/opt/MiniCollection/data/Config.ini
/opt/MiniCollection/data/weizhen.db
/opt/MiniCollection/data/temp
/opt/MiniCollection/data/picker
/opt/MiniCollection/data/bins
/opt/MiniCollection/Log
```

这些目录保存配置、数据库、日志和采集数据，后续更新时不要删除或覆盖。

## 五、日常更新部署

修改代码并成功编译后，在编译机执行：

在编译机执行：
```bash
cd "$HOME/work/MiniCollection"

scp build-eg628-qt6/MiniCollection \
    build-eg628-qt6/MiniWatchdog \
    root@192.168.111.2:/tmp/
```
然后停止服务、替换程序：
```bash
ssh root@192.168.111.2 '
  set -e

  systemctl stop minicollection.service

  pkill -TERM MiniWatchdog 2>/dev/null || true
  pkill -TERM MiniCollection 2>/dev/null || true

  sleep 2

  install -m 755 /tmp/MiniCollection \
    /opt/MiniCollection/MiniCollection

  install -m 755 /tmp/MiniWatchdog \
    /opt/MiniCollection/MiniWatchdog

  systemctl start minicollection.service
  systemctl --no-pager --full status minicollection.service
'
```
确认新程序已经运行：
```bash
ssh root@192.168.111.2 '
  ps -ef | grep -E "[M]iniWatchdog|[M]iniCollection"
  journalctl -u minicollection.service -n 50 --no-pager
'
```

## 六、配置 systemd 开机启动

以下命令在目标机执行。

创建服务文件：

```bash
cat >/etc/systemd/system/minicollection.service <<'EOF'
[Unit]
Description=MiniCollection acquisition service
After=network-online.target display-manager.service
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/MiniCollection
ExecStart=/opt/MiniCollection/MiniWatchdog
Restart=always
RestartSec=2
Environment=DISPLAY=:0
Environment=QT_QPA_PLATFORM=xcb

[Install]
WantedBy=graphical.target
EOF
```

上述配置要求目标机已启动 X11 图形服务和窗口管理器。`xcb` 模式下应用是普通窗口，
鼠标指针、窗口边框、移动和缩放由窗口管理器负责。先在目标机的图形桌面终端执行：

```bash
echo "$DISPLAY"
echo "$XAUTHORITY"
ps -ef | grep -E '[X]org|[X]wayland'
```

如果 `DISPLAY` 不是 `:0`，按实际输出修改服务。若图形会话使用 Xauthority，服务还要增加，
路径按 `echo "$XAUTHORITY"` 的实际输出填写：

```ini
Environment=XAUTHORITY=/实际路径/.Xauthority
```

如果目标机没有 X11 和窗口管理器，`eglfs` 与 `linuxfb` 都是直接占用显示设备的模式，
不能提供普通桌面窗口；需要先安装并启动 X11/窗口管理器，才能使用 `xcb`。

加载、启用并立即启动：

```bash
systemctl daemon-reload
systemctl enable --now minicollection.service
systemctl --no-pager --full status minicollection.service
```

注意：以上 `systemctl` 命令必须在 EG628 目标机上执行。编译机上执行会得到
`Access denied` 或 `No such file or directory`，因为编译机没有这个 service 文件。
从编译机远程执行可使用：

```bash
ssh root@192.168.111.2 \
  'systemctl daemon-reload && systemctl enable --now minicollection.service && systemctl is-enabled minicollection.service'
```

验证是否设置成功：

```bash
systemctl is-enabled minicollection.service
systemctl is-active minicollection.service
```

预期分别输出：

```text
enabled
active
```

重启目标机验证开机启动：

```bash
reboot
```

目标机重新联网后，在编译机检查：

```bash
ssh root@192.168.111.2 \
  'systemctl --no-pager --full status minicollection.service'
```

## 七、常用管理命令

以下命令在目标机执行：

```bash
# 查看状态
systemctl status minicollection.service

# 启动
systemctl start minicollection.service

# 停止
systemctl stop minicollection.service

# 重启
systemctl restart minicollection.service

# 查看本次启动后的日志
journalctl -u minicollection.service -b

# 持续查看日志
journalctl -u minicollection.service -f

# 查看最近 100 行
journalctl -u minicollection.service -n 100 --no-pager

# 禁止开机启动并停止程序
systemctl disable --now minicollection.service
```

应用自身日志位于：

```text
/opt/MiniCollection/Log/
```

## 八、更新失败时回滚

目标机执行：

```bash
systemctl stop minicollection.service
cd /opt/MiniCollection
cp -a MiniCollection.bak MiniCollection
chmod 755 MiniCollection
systemctl start minicollection.service
systemctl --no-pager --full status minicollection.service
```

## 九、常见故障

### 程序依赖缺失

```bash
ldd /opt/MiniCollection/MiniCollection | grep 'not found'
```

应把缺少的 AArch64 动态库安装或部署到目标机，不能复制 x86_64 库。

### Qt 平台插件启动失败

```bash
QT_DEBUG_PLUGINS=1 \
DISPLAY=:0 \
QT_QPA_PLATFORM=xcb \
/opt/MiniCollection/MiniCollection
```

窗口化运行必须选择 `xcb`，并确保 X11、窗口管理器、`DISPLAY` 和访问权限均正常。

### 目标机没有接显示器

显示器本身不是 Qt 启动的必要条件，但窗口化的 `xcb` 模式必须有可连接的 X Server。
如果目标机没有显示器但运行了 Xorg 虚拟显示（例如 Xvfb），可以继续使用 `xcb`，并把
`DISPLAY` 设置为该虚拟显示；此时窗口存在于虚拟屏幕上，物理上不可见。

如果目标机没有任何 X Server，只要求后台采集、写文件和网络连接，可以使用 Qt 的
`offscreen` 平台。它不会显示窗口，但程序的 `QApplication` 和业务线程仍可运行。
将服务中的两行替换为：

```ini
Environment=QT_QPA_PLATFORM=offscreen
```

并把启动目标改为 `multi-user.target`：

```ini
[Install]
WantedBy=multi-user.target
```

修改后在目标机执行：

```bash
systemctl daemon-reload
systemctl enable --now minicollection.service
```

`offscreen` 模式下不要期待窗口、鼠标或桌面交互；需要查看界面时，应恢复 `xcb` 并提供
Xorg/Xvfb 及正确的 `DISPLAY`、`XAUTHORITY`。

### 修改服务文件后没有生效

```bash
systemctl daemon-reload
systemctl restart minicollection.service
```

### 确认正在运行的新版本

```bash
stat /opt/MiniCollection/MiniCollection
systemctl show minicollection.service -p MainPID
```

## 十、最短日常操作流程

编译机执行：

```bash
source "$HOME/.bashrc"
cd "$HOME/work/MiniCollection"
cmake --build build-eg628-qt6 --parallel "$(nproc)"

scp build-eg628-qt6/MiniCollection \
    root@192.168.111.2:/tmp/MiniCollection.new

ssh root@192.168.111.2 '
  set -e
  systemctl stop minicollection.service
  cd /opt/MiniCollection
  cp -a MiniCollection MiniCollection.bak
  install -m 755 /tmp/MiniCollection.new MiniCollection
  systemctl start minicollection.service
  systemctl --no-pager --full status minicollection.service
'
```

## 十一、开机自动配置 eth0 和 SSH

如果每次重启后都需要手工执行 `ip link set eth0 nomaster`、`ip addr add` 等命令，
说明网卡配置没有持久化。以下方案使用 systemd 在 SSH 启动前自动执行网络配置。

以下命令在 EG628 目标机执行。先确认网卡名称和当前状态：

```bash
ip -br link
ip -br addr
```

创建配置脚本（按实际网段修改地址；如果不需要网关可删除 `ip route` 一行）：

```bash
cat >/usr/local/sbin/eg628-network.sh <<'EOF'
#!/bin/sh
set -eu

IF=eth0
ADDR=192.168.111.2/24
GATEWAY=192.168.111.1

# eth0 如果曾被加入 bridge，先解除 enslave
ip link set "$IF" nomaster 2>/dev/null || true
ip link set "$IF" up

# 避免重启服务时重复添加地址
ip addr flush dev "$IF"
ip addr add "$ADDR" dev "$IF"

# 没有网关时删除这两行
ip route del default dev "$IF" 2>/dev/null || true
ip route add default via "$GATEWAY" dev "$IF" 2>/dev/null || true
EOF

chmod 755 /usr/local/sbin/eg628-network.sh
```

创建 systemd 服务：

```bash
cat >/etc/systemd/system/eg628-network.service <<'EOF'
[Unit]
Description=EG628 persistent eth0 network configuration
DefaultDependencies=no
After=local-fs.target
Before=network-pre.target network.target ssh.service
Wants=network-pre.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/eg628-network.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
```

加载、启用并立即测试：

```bash
systemctl daemon-reload
systemctl enable eg628-network.service
systemctl restart eg628-network.service
systemctl --no-pager --full status eg628-network.service
ip -br addr show eth0
ip route
```

应看到 `eth0` 有 `192.168.111.2/24`。之后重启验证：

```bash
reboot
```

重新联网后检查：

```bash
systemctl is-enabled eg628-network.service
systemctl is-active eg628-network.service
ip -br addr show eth0
```

如果服务失败，查看：

```bash
journalctl -u eg628-network.service -b --no-pager
```

注意：如果系统由 NetworkManager、netplan 或 systemd-networkd 管理 `eth0`，它们可能在
本服务之后再次覆盖地址。长期推荐把固定地址写入系统原生网络配置，并删除脚本中的重复配置。
如果目标机使用 NetworkManager，可先检查：

```bash
systemctl is-active NetworkManager
nmcli connection show
```

若使用 NetworkManager，可以改用持久连接配置：

```bash
nmcli connection add type ethernet ifname eth0 con-name eg628-eth0 \
  ipv4.method manual ipv4.addresses 192.168.111.2/24 \
  ipv4.gateway 192.168.111.1 ipv4.dns "" \
  connection.autoconnect yes
nmcli connection up eg628-eth0
```

如果已有同名连接，使用 `nmcli connection modify` 修改它，不要重复创建。固定 IP 配置完成
后，SSH 将在每次开机自动恢复；`minicollection.service` 会在网络目标达到后启动。

## 十二、编译时固定设备编号

每台 EG628 必须使用唯一设备编号，例如 `01`、`02`。编号在编译时写入程序并保留前导零，不能在
目标机的 `Config.ini` 中修改。

第一次为设备 `01` 配置已有构建目录：

```bash
source "$HOME/.bashrc"
cd "$HOME/work/MiniCollection"

cmake -S . -B build-eg628-qt6 \
  -DQT_HOST_MOC="$HOME/bin/moc-eg628" \
  -DMINICOLLECTION_DEVICE_ID=01 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-eg628-qt6 --parallel "$(nproc)"
```

现有 `build-eg628-qt6` 已缓存工具链、sysroot 和 Qt 路径，不要在环境变量为空时重新创建构建目录。
检查生成的身份：

```bash
grep MINICOLLECTION_DEVICE_ID build-eg628-qt6/CMakeCache.txt
grep QStringLiteral build-eg628-qt6/DeviceIdentity.h
```

应分别显示 `01`。同一构建目录改成设备 `02` 会覆盖上次的编译身份；批量维护设备时应为每个编号
建立独立且已正确配置 Qt/sysroot 的构建目录。

## 十三、云端上传、参数同步和状态心跳

目标机 `/opt/MiniCollection/data/Config.ini` 至少包含：

```ini
[Upload]
enabled=1
pollSec=5
serverUrl=http://39.96.41.239:443
token=服务器上传令牌
```

`serverUrl` 同时用于 mseed 上传、参数拉取和状态心跳。多项目部署时，每台设备只填写自己项目的
`serverUrl` 和上传令牌；详见 [`Mseed服务器部署与接口协议.md`](Mseed服务器部署与接口协议.md) 第十一节。
程序每 10 秒执行一次配置查询和状态上报：

- 从 `/coalmine/devices/设备编号/config/latest` 获取最新配置；
- 在算法线程计算边界整组应用 `[Para]`，不重启程序；
- 应用成功后同步刷新设备界面的参数输入框和上传地址；
- 已应用版本只保存在进程内存中，不写入 `Config.ini`；设备重启后会重新应用服务端最新配置；
- 向服务器上报设备 IP、程序版本、各站连接、在线通道和 GPS 状态；
- 应用回执因断网失败时自动重试。

mseed 在服务器按设备保存到 `files/01/`、`files/02/` 等目录；目标机本地上传状态也按设备保存在：

```text
/opt/MiniCollection/data/upload/01/progress.ini
/opt/MiniCollection/data/upload/01/done.txt
```

## 十四、443 端口检查

当前部署是明文 HTTP 运行在 443 端口，不是 HTTPS。服务器 Nginx 必须包含：

```nginx
listen 80;
listen 443;
```

服务器检查：

```bash
sudo nginx -t
sudo systemctl reload nginx
sudo ss -lntp | grep ':443'
curl http://127.0.0.1:443/healthz
```

EG628 检查：

```bash
curl -v --connect-timeout 10 http://39.96.41.239:443/healthz
curl -v --connect-timeout 10 http://39.96.41.239:443/coalmine/devices/01
```

如果日志出现 `HTTP=0 Connection refused`，说明 TCP 端口未监听或被防火墙/安全组拒绝，不是参数
接口错误。确认 Nginx、系统防火墙和云安全组均开放 TCP 443。地址必须以 `http://` 开头。

## 十五、当前版本完整更新流程

编译机执行：

```bash
source "$HOME/.bashrc"
cd "$HOME/work/MiniCollection"

cmake -S . -B build-eg628-qt6 \
  -DQT_HOST_MOC="$HOME/bin/moc-eg628" \
  -DMINICOLLECTION_DEVICE_ID=01 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-eg628-qt6 --parallel "$(nproc)"

scp build-eg628-qt6/MiniCollection \
    build-eg628-qt6/MiniWatchdog \
    root@192.168.111.2:/tmp/
```

目标机替换并验证：

```bash
systemctl stop minicollection.service
install -o root -g root -m 755 /tmp/MiniCollection /opt/MiniCollection/MiniCollection
install -o root -g root -m 755 /tmp/MiniWatchdog /opt/MiniCollection/MiniWatchdog
systemctl start minicollection.service

systemctl --no-pager --full status minicollection.service
journalctl -u minicollection.service -n 100 --no-pager
```

等待约 10 秒后在服务器检查设备状态：

```bash
curl http://127.0.0.1:9001/coalmine/devices/01
```

应看到 `online:true`，并包含 `status.stations`、`applied_config_version` 等字段。
