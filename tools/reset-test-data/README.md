# 清空测试数据脚本

用于把**控制台、服务器、采集设备**三端的测试 mseed、上传状态和下载游标清理干净。

> 这些脚本会删除文件，先加 `-DryRun`（Windows）或 `--dry-run`（Linux）确认路径。
> 建议清理顺序：**控制台 → 设备 → 服务器**，然后按服务器、设备、控制台的顺序启动。

## 1. 控制台（Windows）

脚本：`reset-console.ps1`

清理内容：

- `[Consumer] stateFile` 下的所有 `cursor.txt`（下载游标）；
- `[Consumer] outputDir` 下的所有 `*.mseed` / `*.mseed.part`；
- `[DeviceDirs]` 中每台设备单独目录下的所有 `*.mseed` / `*.mseed.part`；
- 自动停止 `MseedConsumer.exe`。

先预览：

```powershell
powershell -ExecutionPolicy Bypass -File .\reset-console.ps1 `
  -ConsoleDir "D:\MseedConsumer\dist-win64" -DryRun
```

确认后执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\reset-console.ps1 `
  -ConsoleDir "D:\MseedConsumer\dist-win64"
```

脚本会要求输入 `RESET` 二次确认。

注意：

- 不会删除 `config.ini`；
- `[DeviceDirs]` 指向的目录必须只用于 mseed 测试数据，脚本会删除其中所有 mseed 文件；
- 删除游标后，控制台会从 ID 0 重新下载服务器上现有的文件。

## 2. 采集设备（Linux）

脚本：`reset-device.sh`

清理内容：

- `data/upload/` 下所有设备的上传进度和 done 记录；
- `data/picker/*.mseed`；
- `data/temp/*.mseed`；
- `data/invalid/*.mseed`；
- 可选：`data/bins/` 连续原始数据（加 `--include-bins`）；
- 清理期间停止 `minicollection.service`，清理后自动启动。

上传到设备后执行：

```bash
chmod +x reset-device.sh

# 预览
sudo ./reset-device.sh --dry-run

# 清理，保留 data/bins
sudo ./reset-device.sh

# 连 data/bins 也清理
sudo ./reset-device.sh --include-bins
```

如果程序安装目录不是 `/opt/MiniCollection`：

```bash
sudo ./reset-device.sh --app-dir /your/path/MiniCollection
```

脚本不会删除 `Config.ini`、`weizhen.db`、程序文件和日志。

## 3. 服务器（Linux）

脚本：`reset-server.sh`

默认项目 `project_lgy`，清理内容：

- 该项目 `MSEED_DATA_ROOT/files/` 下所有完整 mseed；
- `MSEED_DATA_ROOT/parts/` 下所有未完成分片；
- 数据库 `files` 表记录（默认保留设备和配置表）；
- 可选 `--reset-db`：删除整个 `mseed.db`，设备配置、参数历史和文件记录全部重置。

上传到服务器后执行：

```bash
chmod +x reset-server.sh

# 预览
sudo ./reset-server.sh project_lgy --dry-run

# 只清文件和文件记录，保留设备配置
sudo ./reset-server.sh project_lgy

# 完全重置项目数据库
sudo ./reset-server.sh project_lgy --reset-db
```

脚本会先停止 `mseed-server-project_lgy.service`，备份数据库到 `/root/`，清理后重新启动。

注意：

- 默认只删除 `files` 表记录，保留 `devices` 和 `device_configs`；
- `--reset-db` 会让设备配置、参数版本、设备列表一起消失，设备下次心跳会重新创建设备；
- 不会删除 `/etc/mseed-server-project_lgy.env` 和项目程序；
- 只会清理指定的一个项目，不影响其它项目。

## 4. 完整重置流程

```text
1. 控制台：运行 reset-console.ps1
2. 设备：  sudo ./reset-device.sh
3. 服务器：sudo ./reset-server.sh project_lgy
4. 启动服务器：sudo systemctl start mseed-server-project_lgy.service
5. 启动设备：  sudo systemctl start minicollection.service
6. 启动控制台：MseedConsumer.exe
```

如果服务器使用了 `--reset-db`，控制台游标必须已经清零，否则会再次出现“服务器有新文件但控制台取不到”的现象。

## 5. 只清游标，不删服务器/设备数据

如果只想让控制台重新下载现有文件，只运行控制台脚本即可：

```powershell
powershell -ExecutionPolicy Bypass -File .\reset-console.ps1 `
  -ConsoleDir "D:\MseedConsumer\dist-win64"
```

服务器和设备的数据保持不动。
