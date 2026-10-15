# EG628 新机网口与 SSH 配置手册

适用环境：EG628、Ubuntu 20.04、网口 `eth0`。设备固定地址为 `192.168.111.2/24`，Windows 电脑可使用 `192.168.111.12/24`。本方案把 eth0 作为独立管理口，每次开机约 30 秒执行一次网络恢复，不做每 15 秒的周期检查。

厂家固件可能把 eth0 加入 wan 网桥。本方案解除该关系，因此会改变 eth0 原有的厂家桥接功能。开机延时是当前设备的实用兜底方案，不是厂家网络管理配置的替代品；新机固件不同或厂家程序晚于 30 秒重新接管网口时，需要进一步调整厂家配置。

## 1. 首次连接设备

用网线连接 Windows 和设备。首次配置通过设备的显示器、键盘或串口本地终端执行，避免网络变更导致 SSH 中断。

在 Windows 的“网络连接 → 对应以太网适配器 → IPv4 属性”设置：

| 项目 | 值 |
| --- | --- |
| IP 地址 | 192.168.111.12 |
| 子网掩码 | 255.255.255.0 |
| 默认网关 | 留空 |
| DNS | 留空 |

电脑与设备不能使用同一个 IP。该直连管理网口不需要默认网关；电脑其他网卡可以继续提供上网连接。

## 2. 临时恢复设备网络

以下命令在目标机执行。已经是 root 时不需要 sudo：

```bash
sudo -i
ip link show eth0
ip link set dev eth0 nomaster
ip link set dev eth0 up
ip addr replace 192.168.111.2/24 dev eth0
mkdir -p /run/sshd
/usr/sbin/sshd -t
systemctl enable ssh.service
systemctl restart ssh.service
```

`sshd -t` 正常时没有输出；若报错，先修复该错误。不要执行 `ip addr flush` 或 `iptables -F`：前者会移除地址，后者清空整个防火墙。

如果防火墙阻止 SSH，执行以下命令，仅放行 eth0 上来自管理网段的 SSH 及回复：

```bash
iptables -C INPUT -i eth0 -s 192.168.111.0/24 -p tcp --dport 22 -j ACCEPT 2>/dev/null ||
iptables -I INPUT 1 -i eth0 -s 192.168.111.0/24 -p tcp --dport 22 -j ACCEPT

iptables -C OUTPUT -o eth0 -d 192.168.111.0/24 -p tcp --sport 22 -m conntrack --ctstate ESTABLISHED -j ACCEPT 2>/dev/null ||
iptables -I OUTPUT 1 -o eth0 -d 192.168.111.0/24 -p tcp --sport 22 -m conntrack --ctstate ESTABLISHED -j ACCEPT
```

Windows PowerShell 测试：

```powershell
Test-NetConnection 192.168.111.2 -Port 22
ssh root@192.168.111.2
```

TCP 测试成功但提示 `Permission denied`，说明网络已通，应检查 SSH 账号和认证配置；新固件不一定允许 root 密码登录，可使用厂家提供的账号。

## 3. 创建开机配置脚本

以下全部在目标机 root 终端执行，不是在 Windows 或编译虚拟机执行。

已有同名文件时先备份（新机没有文件时自动跳过）：

```bash
backup_dir="/root/eg628-network-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup_dir"
for path in /usr/local/sbin/eg628-network-init.sh /etc/systemd/system/eg628-network-check.service /etc/systemd/system/eg628-network-check.timer; do
    if [ -f "$path" ]; then
        cp -a "$path" "$backup_dir/"
    fi
done
```

创建脚本：

```bash
mkdir -p /usr/local/sbin
tee /usr/local/sbin/eg628-network-init.sh >/dev/null <<'EOF'
#!/bin/sh
set -eu

ip link set dev eth0 nomaster
ip link set dev eth0 up
ip addr replace 192.168.111.2/24 dev eth0
mkdir -p /run/sshd

# 防火墙失败会记录警告，但不阻止前面的地址设置。
if command -v iptables >/dev/null 2>&1; then
    if ! iptables -C INPUT -i eth0 -s 192.168.111.0/24 -p tcp --dport 22 -j ACCEPT 2>/dev/null; then
        iptables -I INPUT 1 -i eth0 -s 192.168.111.0/24 -p tcp --dport 22 -j ACCEPT ||
            echo 'WARNING: failed to allow SSH input' >&2
    fi
    if ! iptables -C OUTPUT -o eth0 -d 192.168.111.0/24 -p tcp --sport 22 -m conntrack --ctstate ESTABLISHED -j ACCEPT 2>/dev/null; then
        iptables -I OUTPUT 1 -o eth0 -d 192.168.111.0/24 -p tcp --sport 22 -m conntrack --ctstate ESTABLISHED -j ACCEPT ||
            echo 'WARNING: failed to allow SSH output' >&2
    fi
fi
EOF
chmod 755 /usr/local/sbin/eg628-network-init.sh
```

脚本不清空现有地址、不添加默认网关，也不反复重启 SSH。若网卡不存在或设置失败，服务会报告失败。

## 4. 配置开机只执行一次

创建单次执行服务：

```bash
tee /etc/systemd/system/eg628-network-check.service >/dev/null <<'EOF'
[Unit]
Description=Restore EG628 eth0 management address
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/eg628-network-init.sh
TimeoutStartSec=10
EOF
```

创建定时器：

```bash
tee /etc/systemd/system/eg628-network-check.timer >/dev/null <<'EOF'
[Unit]
Description=Configure EG628 management network once after boot

[Timer]
OnBootSec=30s
AccuracySec=1s
Unit=eg628-network-check.service

[Install]
WantedBy=timers.target
EOF
```

这里没有 `OnUnitInactiveSec=15s`，因此不周期执行。定时器在开机约 30 秒后触发一次；启用时若已超过该时间，可能立即触发。

如果以前创建过其他网络修复服务，先取消其开机启动，保留文件方便回退：

```bash
for unit in eg628-netfix.service eg628-network.service; do
    if systemctl cat "$unit" >/dev/null 2>&1; then
        systemctl disable "$unit"
    fi
done
```

如果 MiniCollection 以前依赖 `eg628-network.service`，上述 disable 不会阻止它通过依赖再次启动。检查：

```bash
systemctl cat minicollection.service
```

若 `/etc/systemd/system/minicollection.service.d/network.conf` 仅包含此前添加的 `Requires=eg628-network.service`、`After=eg628-network.service`，可将该文件移入备份目录：

```bash
mv /etc/systemd/system/minicollection.service.d/network.conf "$backup_dir/minicollection-network.conf"
```

仅在确认该文件内容符合上述情况后执行；其他依赖配置不要直接删除。此次网络配置不会承诺采集程序等到网络修复完成后才启动。

加载、检查并启用：

```bash
systemctl daemon-reload
systemd-analyze verify /etc/systemd/system/eg628-network-check.service /etc/systemd/system/eg628-network-check.timer
systemctl enable ssh.service
systemctl restart eg628-network-check.timer
systemctl enable eg628-network-check.timer
systemctl start eg628-network-check.service
```

最后一条是安装时的手动测试；之后每次开机由定时器执行一次。

## 5. 检查与重启验证

目标机执行：

```bash
ip -4 addr show eth0
ss -lntp | grep ':22'
systemctl is-enabled eg628-network-check.timer
systemctl status eg628-network-check.service --no-pager
systemctl list-timers --all eg628-network-check.timer
```

应看到 IP `192.168.111.2/24`、没有 `master wan`、SSH 监听 22、定时器 `enabled`、服务执行结果 `status=0/SUCCESS`。oneshot 服务执行完显示 `inactive (dead)` 正常。单次定时器触发之后也可能显示 `active (elapsed)`，没有下一次触发时间，这是正常的。

保留本地显示器或串口作为恢复入口，重启：

```bash
reboot
```

等待约一分钟，从 Windows 再次执行：

```powershell
Test-NetConnection 192.168.111.2 -Port 22
ssh root@192.168.111.2
```

连接成功后查看本次开机记录：

```bash
ip -4 addr show eth0
journalctl -b -u eg628-network-check.service --no-pager
```

等待几分钟再检查，确认没有重新出现 `master wan`。在未手动运行修复命令的情况下重启后能直接 SSH，才算开机验证通过。

## 6. 常见问题与撤销

| 现象 | 检查方法 |
| --- | --- |
| eth0 显示 NO-CARRIER | 检查网线、网口和电脑以太网适配器 |
| 没有固定 IP | 查看 `journalctl -b -u eg628-network-check.service` |
| 又出现 master wan | 厂家程序晚于单次修复重新接管，需修改厂家网络配置；增加延时只能兜底 |
| Connection refused | 检查 `systemctl status ssh`、`ss -lntp`、`/usr/sbin/sshd -t` |
| Connection timed out | 检查电脑网段、重复 IP、路由、防火墙和网线 |
| heredoc 提示 No space left on device | 用 `df -h / /tmp` 查空间；不要整目录清空 /tmp，只处理确认无用的文件 |

厂家初始化入口可能是 `/etc/rc.local` 后台启动 `/etc/rcS`，再运行 `m300_script` 内的程序，包括 `netservice.out`。不要直接禁用整个厂家启动流程，也不要重新执行 rc.local，其中可能包含分区格式化和驱动初始化操作。

取消以后开机的单次修复：

```bash
systemctl disable --now eg628-network-check.timer
```

这不会撤销已经设置的 IP、网桥关系或防火墙规则。若要恢复厂家网络，应通过其配置入口处理，并在本地终端操作。
