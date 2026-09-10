# MiniCollection — 精简版微震数据采集程序

从 CollectionSystem 项目中提取核心功能逻辑，去掉复杂界面、波形显示、用户管理、
CAN 管理、系统设置等模块，仅保留最小数据链路：

```
TcpConnection(每站) -> TcpDataManager(按包首时间排序分发)
    -> FrameParser(局域网/云端协议解析)
        -> MDataPickder(跨站同步对齐 -> STALTA算法 -> 事件mseed -> data/temp)
        -> MDataWriter(站0连续原始数据 -> data/bins)
MseedFileHandler: data/temp -> data/picker(有效事件) / data/invalid(无效)
HttpMgr: 计算窗口JSON上传(可配)
```

## 界面（原生控件，无自定义样式）
- 表格：每站 连接状态 / 在线通道数 / GPS / 最近事件
- 同步状态行（等待数据 / 同步正常 / 同步异常 + 最大时间差）
- 算法参数：tri_on / tri_off / nsta / nlta / detect_ch / qualified_ch /
  energy_thre / amp_thre / intrach / interch（写入 Config.ini [Para] 并即时生效）
- 滚动日志

## 构建
```
cmake -B build -G "Visual Studio 16 2019" -A Win32 ^
      -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019
cmake --build build --config Release
```
产物：`MiniCollection.exe`（主程序）+ `MiniWatchdog.exe`（看门狗）。
部署时两者放在同一目录。依赖：Qt 5.15 (Core/Gui/Widgets/Network/Sql)、
libmseed（复用 `../thirdpartyLib/mseed`）。

## 看门狗（MiniWatchdog）
从原项目 `src/watchdog` 移植，行为一致：
- 启动并守护同目录下的 `MiniCollection.exe`
- 进程**崩溃退出**(CrashExit)时自动重启：最多100次，间隔2秒，重启时带 `--restore-state`
- 可选 UDP 心跳检测（默认关闭，`setHeartbeatEnabled(true, 12345)` 开启）

## 参数初始化
启动时（界面创建之前）检查 `data/Config.ini` 的 `[Para]` 段，
缺失的项自动写入与原项目一致的默认值：
`tri_on=6, tri_off=2, nsta=50, nlta=500, detect_ch=4, qualified_ch=4,
energy_thre=0.25, amp_thre=0.25, intrach=100, interch=250, calc_win_len=2500`。
界面初始化时读取这些参数填入输入框，可修改后点"应用"即时生效到算法。

## 配置
- `data/Config.ini`
  - `[Sver1..N] IP/Port`：站点连接（或数据库 `tbl_station`，优先数据库）
  - `[Para]`：算法参数（也可在界面修改）
- 数据库 `data/weizhen.db`：站点/分量配置（`tbl_station` 等，启动时自动建表补齐）

## 与原项目的差异
- 无登录流程，程序启动即连接所有站点并开始采集
- 无波形绘制（qcustomplot 等未引入），在线状态用表格显示
- 分量模式固定为"自动"（按数据帧内类型位解析单/三分量）
- 不含 CAN 管理、看门狗、备份机同步、用户/角色管理
