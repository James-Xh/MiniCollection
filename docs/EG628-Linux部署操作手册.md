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

```bash
cd "$HOME/work/MiniCollection"

scp build-eg628-qt6/MiniCollection \
    root@192.168.111.2:/tmp/MiniCollection.new
```

然后安全停止、备份、替换并启动：

```bash
ssh root@192.168.111.2 '
  set -e
  systemctl stop minicollection.service
  cd /opt/MiniCollection
  [ ! -f MiniCollection ] || cp -a MiniCollection MiniCollection.bak
  install -m 755 /tmp/MiniCollection.new MiniCollection
  systemctl start minicollection.service
  systemctl --no-pager --full status minicollection.service
'
```

更新过程只替换 `MiniCollection`，不会影响 `data` 和 `Log`。

如果同时修改了看门狗，再单独上传：

```bash
scp build-eg628-qt6/MiniWatchdog \
    root@192.168.111.2:/opt/MiniCollection/
```

systemd 已负责异常重启，Linux 部署通常不需要启动 `MiniWatchdog`。

## 六、配置 systemd 开机启动

以下命令在目标机执行。

创建服务文件：

```bash
cat >/etc/systemd/system/minicollection.service <<'EOF'
[Unit]
Description=MiniCollection acquisition service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/MiniCollection
ExecStart=/opt/MiniCollection/MiniCollection
Restart=on-failure
RestartSec=2
Environment=QT_QPA_PLATFORM=eglfs

[Install]
WantedBy=multi-user.target
EOF
```

如果目标系统使用 X11 桌面，把服务中的环境配置改为：

```ini
Environment=DISPLAY=:0
Environment=QT_QPA_PLATFORM=xcb
```

如果目标系统直接使用 framebuffer，但 EGLFS 不适用，可改为：

```ini
Environment=QT_QPA_PLATFORM=linuxfb
```

加载、启用并立即启动：

```bash
systemctl daemon-reload
systemctl enable --now minicollection.service
systemctl --no-pager --full status minicollection.service
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
QT_QPA_PLATFORM=eglfs \
/opt/MiniCollection/MiniCollection
```

根据实际显示环境在 `eglfs`、`linuxfb`、`xcb` 中选择，并同步修改 systemd 服务。

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

