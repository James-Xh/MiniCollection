# MiniCollection 部署到 EG628 AArch64：从新机开始

本文记录从一台新 Ubuntu x86_64 编译机开始，将 MiniCollection 部署到 EG628
AArch64 目标机的完整流程。

## 1. 已验证环境

编译机：Ubuntu 22.04 x86_64，CMake 3.22，Ninja 1.10。

目标机：AArch64、64 位、glibc 2.31、Qt 5.12.8，IP `192.168.111.2`。

命令提示符含义：

```text
xuhao@xuhao:~$   Ubuntu 编译机
root@EG628:~#    EG628 目标机
C:\Users\...>   Windows 主机，不执行 Linux 命令
```

## 2. 编译机安装工具

```bash
sudo apt update
sudo apt install -y cmake ninja-build build-essential pkg-config qemu-user
```

确认：

```bash
cmake --version
ninja --version
qemu-aarch64 --version
```

如果 apt 提示 dpkg 锁被自动更新占用，等待 `unattended-upgrades` 完成；不要删除锁文件。

## 3. 安装交叉工具链

将厂家或 Linaro 工具链放到编译机，例如：

```text
$HOME/gcc-linaro-10.2.1-2021.01-x86_64_aarch64-linux-gnu
```

设置环境变量：

```bash
export EG628_TC="$HOME/gcc-linaro-10.2.1-2021.01-x86_64_aarch64-linux-gnu"
export PATH="$EG628_TC/bin:$PATH"
export CROSS_COMPILE=aarch64-linux-gnu-
```

验证：

```bash
${CROSS_COMPILE}gcc -dumpmachine
${CROSS_COMPILE}gcc --version
${CROSS_COMPILE}gcc -print-sysroot
```

目标三元组应为 `aarch64-linux-gnu`。工具链自带 sysroot 是 glibc 2.32，不能直接用于目标机
glibc 2.31，因此后续必须使用从 EG628 导出的目标 sysroot。

## 4. 确认目标机 ABI

在 EG628 控制台执行：

```bash
uname -m
getconf LONG_BIT
getconf GNU_LIBC_VERSION
file /bin/ls
readelf -l /bin/ls | grep 'Requesting program interpreter'
```

应确认：`aarch64`、`64`、`glibc 2.31` 和 `/lib/ld-linux-aarch64.so.1`。

## 5. 从目标机制作本地 sysroot

目标机必须有头文件和启动文件。先在目标机检查：

```bash
test -f /usr/include/stdio.h && echo headers-present
find /usr /lib -type f \
  \( -name crt1.o -o -name crti.o -o -name crtn.o \) 2>/dev/null
```

在编译机执行以下命令。它通过 SSH 管道传输，不会占用目标机 `/tmp`：

```bash
export TARGET_SYSROOT="$HOME/eg628-sysroot"
mkdir -p "$TARGET_SYSROOT"

ssh root@192.168.111.2 '
  cd /
  tar -cf - --numeric-owner lib usr/lib usr/include
' | tar -xf - -C "$TARGET_SYSROOT"
```

确认：

```bash
find "$TARGET_SYSROOT" -name 'libc-2.31.so' -o -name 'libc.so.6'
find "$TARGET_SYSROOT" -name 'ld-linux-aarch64.so.1'
find "$TARGET_SYSROOT" -name 'crt1.o' -o -name 'crti.o' -o -name 'crtn.o'
test -f "$TARGET_SYSROOT/usr/include/stdio.h" && echo headers-present
```

如果目标机没有头文件或 `crt*.o`，必须向厂家索取完整 SDK/sysroot，不能只使用运行时库。

## 6. 复制目标机 Qt 5.12.8

目标机应存在：

```text
/usr/include/aarch64-linux-gnu/qt5
/usr/lib/aarch64-linux-gnu/cmake
/usr/lib/aarch64-linux-gnu/qt5
/usr/lib/aarch64-linux-gnu/libQt5*.so*
```

在编译机执行：

```bash
ssh root@192.168.111.2 '
  cd /
  tar -cf - --numeric-owner \
    usr/include/aarch64-linux-gnu/qt5 \
    usr/lib/aarch64-linux-gnu/cmake \
    usr/lib/aarch64-linux-gnu/qt5
' | tar -xf - -C "$TARGET_SYSROOT"

ssh root@192.168.111.2 '
  cd /
  find usr/lib/aarch64-linux-gnu -maxdepth 1 \
    \( -type f -o -type l \) -name "libQt5*" -print0 |
  tar --null -T - -cf - --numeric-owner
' | tar -xf - -C "$TARGET_SYSROOT"
```

确认：

```bash
export Qt5_DIR="$TARGET_SYSROOT/usr/lib/aarch64-linux-gnu/cmake/Qt5"
test -f "$Qt5_DIR/Qt5Config.cmake" && echo Qt5-config-present
test -f "$TARGET_SYSROOT/usr/include/aarch64-linux-gnu/qt5/QtCore/qglobal.h" && echo Qt5-headers-present
```

Qt 的 CMake 文件有时写死 `/usr/lib/aarch64-linux-gnu`。若配置时报 `libEGL.so` 等文件不存在，
只修改本地 sysroot 副本：

```bash
QT_CMAKE_ROOT="$TARGET_SYSROOT/usr/lib/aarch64-linux-gnu/cmake"
find "$QT_CMAKE_ROOT" -path '*/Qt5*' -type f -name '*.cmake' \
  -exec sed -i \
  "s|\"/usr/lib/aarch64-linux-gnu|\"$TARGET_SYSROOT/usr/lib/aarch64-linux-gnu|g" {} +
```

## 7. 为 AUTOMOC 准备 Qt host tool

目标机的 `moc` 是 AArch64，不能直接在 x86_64 编译机执行。使用 QEMU 包装：

```bash
mkdir -p "$HOME/bin"
cat >"$HOME/bin/moc-eg628" <<EOF
#!/bin/sh
exec /usr/bin/qemu-aarch64 -L "$TARGET_SYSROOT" \
  "$TARGET_SYSROOT/usr/lib/qt5/bin/moc" "\$@"
EOF
chmod +x "$HOME/bin/moc-eg628"
"$HOME/bin/moc-eg628" -v
```

应输出 `moc 5.12.8`。

## 8. 重新编译 AArch64 libmseed

仓库原有 `mseed/libs_linux/libmseed.a` 是 x86_64，必须用 libmseed 3.1.6 源码重编译。

```bash
cd "$HOME/work/libmseed-3.1.6"
make clean
make -j"$(nproc)" \
  CC=aarch64-linux-gnu-gcc \
  AR=aarch64-linux-gnu-ar \
  RANLIB=aarch64-linux-gnu-ranlib \
  CFLAGS="--sysroot=$TARGET_SYSROOT -O2 -fPIC"
```

验证归档中的目标文件：

```bash
mkdir -p /tmp/check-libmseed-aarch64
cd /tmp/check-libmseed-aarch64
aarch64-linux-gnu-ar x "$HOME/work/libmseed-3.1.6/libmseed.a"
file ./*.o | head
```

必须全部显示 `ARM aarch64`。复制到项目的架构专用目录：

```bash
mkdir -p "$HOME/work/MiniCollection/mseed/libs_linux/aarch64"
cp "$HOME/work/libmseed-3.1.6/libmseed.a" \
  "$HOME/work/MiniCollection/mseed/libs_linux/aarch64/libmseed.a"
```

`CMakeLists.txt` 的 Linux 分支应在 AArch64 时使用该路径：

```cmake
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(MSEED_LIB "${MSEED_DIR}/libs_linux/aarch64/libmseed.a")
else()
    set(MSEED_LIB "${MSEED_DIR}/libs_linux/libmseed.a")
endif()
```

## 9. CMake 工具链文件

项目文件 `cmake/toolchains/eg628-aarch64.cmake` 应包含：

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(TOOLCHAIN_ROOT "$ENV{EG628_TC}")
set(TARGET_TRIPLE aarch64-linux-gnu)
set(TARGET_SYSROOT "$ENV{TARGET_SYSROOT}")
set(CMAKE_C_COMPILER "${TOOLCHAIN_ROOT}/bin/${TARGET_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_ROOT}/bin/${TARGET_TRIPLE}-g++")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_ROOT}/bin/${TARGET_TRIPLE}-gcc")
set(CMAKE_SYSROOT "${TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${TARGET_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
```

## 10. 首次交叉编译

项目的 `CMakeLists.txt` 需要：

- `find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Network Sql)`；
- Linux 使用 AArch64 `libmseed.a`；
- `TcpConnection` 对 Qt 5.12 使用 `QAbstractSocket::error`，不能只使用 Qt 5.15 的 `errorOccurred`；
- AUTOMOC 使用 `$HOME/bin/moc-eg628`。

编译机执行：

```bash
cd "$HOME/work/MiniCollection"
cmake -S . -B build-eg628-qt6 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/eg628-aarch64.cmake" \
  -DQt5_DIR="$Qt5_DIR" \
  -DCMAKE_PREFIX_PATH="$TARGET_SYSROOT/usr" \
  -DQT_HOST_MOC="$HOME/bin/moc-eg628" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-eg628-qt6 --parallel "$(nproc)"
```

检查产物：

```bash
file build-eg628-qt6/MiniCollection build-eg628-qt6/MiniWatchdog
```

两者都必须是 `ELF 64-bit ... ARM aarch64`。

## 11. 首次部署和窗口化启动

在编译机上传到目标机临时目录：

```bash
scp build-eg628-qt6/MiniCollection \
    build-eg628-qt6/MiniWatchdog \
    root@192.168.111.2:/tmp/
```

目标机执行：

```bash
mkdir -p /opt/MiniCollection
install -m 755 /tmp/MiniCollection /opt/MiniCollection/MiniCollection
install -m 755 /tmp/MiniWatchdog /opt/MiniCollection/MiniWatchdog
```

看门狗 Linux 版本必须启动 `MiniCollection`，Windows 版本才启动 `MiniCollection.exe`。
systemd 应启动 `MiniWatchdog`，而不是直接启动主程序。

## 12. 网络开机自动配置

目标机如果每次启动都需要手工配置 `eth0`，先通过本地控制台或串口恢复地址，然后创建持久服务。
不要远程执行会包含 `ip addr flush dev eth0` 的命令，否则当前 SSH 会立即断开。

```bash
cat >/usr/local/sbin/eg628-network.sh <<'EOF'
#!/bin/sh
set -eu
IF=eth0
ADDR=192.168.111.2/24
GATEWAY=192.168.111.1
ip link set "$IF" nomaster 2>/dev/null || true
ip link set "$IF" up
ip addr flush dev "$IF"
ip addr add "$ADDR" dev "$IF"
ip route del default dev "$IF" 2>/dev/null || true
ip route add default via "$GATEWAY" dev "$IF" 2>/dev/null || true
EOF
chmod 755 /usr/local/sbin/eg628-network.sh

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

systemctl daemon-reload
systemctl enable eg628-network.service
```

首次配置后重启，让服务在 SSH 启动前配置地址：

```bash
reboot
```

如果没有网关，删除脚本中的默认路由两行。若目标机由 NetworkManager 管理，优先用 `nmcli` 配置
持久连接，避免 NetworkManager 覆盖脚本设置。

## 13. systemd 启动看门狗和窗口

目标机存在 X Server 时，使用 `xcb` 普通窗口模式。确认：

```bash
ls -l /tmp/.X11-unix/X0
DISPLAY=:0 xdpyinfo >/dev/null && echo X11_OK
```

创建服务：

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

systemctl daemon-reload
systemctl enable --now minicollection.service
systemctl --no-pager --full status minicollection.service
```

这些 `systemctl` 命令必须在 `root@EG628` 上执行。若从编译机执行，使用：

```bash
ssh root@192.168.111.2 \
  'systemctl daemon-reload && systemctl enable --now minicollection.service'
```

无物理显示器时，只要 X Server 仍运行，`xcb` 仍可工作；接入显示器后可看到窗口。
没有 X Server 时，改用 `QT_QPA_PLATFORM=offscreen`，程序可后台采集但不会显示窗口，且安装目标应为
`multi-user.target`。

## 14. 修改代码后的更新流程

编译机：

```bash
source "$HOME/.bashrc"
cd "$HOME/work/MiniCollection"
cmake --build build-eg628-qt6 --parallel "$(nproc)"
scp build-eg628-qt6/MiniCollection \
    build-eg628-qt6/MiniWatchdog \
    root@192.168.111.2:/tmp/
```

目标机：

```bash
systemctl stop minicollection.service
cp -a /opt/MiniCollection/MiniCollection \
      /opt/MiniCollection/MiniCollection.bak
install -m 755 /tmp/MiniCollection /opt/MiniCollection/MiniCollection
install -m 755 /tmp/MiniWatchdog /opt/MiniCollection/MiniWatchdog
systemctl start minicollection.service
systemctl --no-pager --full status minicollection.service
```

不能直接用 `scp` 覆盖正在运行的 `/opt/MiniCollection/MiniWatchdog`，否则会出现 `Text file busy`。
不要删除或覆盖 `/opt/MiniCollection/data` 和 `/opt/MiniCollection/Log`。

## 15. 故障排查

```bash
# 服务状态和日志
systemctl status minicollection.service
journalctl -u minicollection.service -n 100 --no-pager

# 进程
ps -ef | grep -E '[M]iniWatchdog|[M]iniCollection'

# 动态库
ldd /opt/MiniCollection/MiniCollection | grep 'not found'

# 开机网络服务
systemctl status eg628-network.service
journalctl -u eg628-network.service -b --no-pager
ip -br addr show eth0
```

如果 SSH 因网络配置断开，使用目标机本地终端或串口执行：

```bash
ip link set eth0 up
ip addr add 192.168.111.2/24 dev eth0
```

如果 Qt 报 `could not connect to display`，检查 `DISPLAY=:0`、X Server、Xauthority 和 `xcb` 插件。
如果编译报 `Exec format error`，检查是否错误执行了 sysroot 中的 AArch64 `moc/uic/rcc`。

