# MseedConsumer

Windows x64 Qt Widgets 消费程序。它按服务端 `id` 游标顺序下载完整 mseed，校验长度和 SHA-256，
原子发布后才保存游标。

## Windows x64 编译

依赖：

- Visual Studio 2019 C++ x64 工具链；
- CMake；
- Qt 5.15.2 MSVC 2019 64-bit，例如 `C:\Qt\5.15.2\msvc2019_64`。

如果 Qt 安装路径不同，修改 `build-windows-x64.bat` 中的 `QT_ROOT`。然后在
`x64 Native Tools Command Prompt for VS 2019` 中执行：

```bat
cd /d D:\work\new\work\MiniCollection\examples\MseedConsumer
build-windows-x64.bat
```

输出目录：

```text
dist-win64\MseedConsumer.exe
```

脚本会调用 `windeployqt`，因此 `dist-win64` 内已经包含运行所需的 Qt DLL，可以复制到其他
Windows x64 机器运行。

也可以手动构建：

```bat
cmake -S . -B build-win64 -G "Visual Studio 16 2019" -A x64 ^
  -DCMAKE_PREFIX_PATH=C:\Qt\5.15.2\msvc2019_64
cmake --build build-win64 --config Release

mkdir dist-win64
copy build-win64\Release\MseedConsumer.exe dist-win64\
C:\Qt\5.15.2\msvc2019_64\bin\windeployqt.exe --release --no-translations ^
  dist-win64\MseedConsumer.exe
```

## 配置

编辑程序同目录的 `config.ini`：

```ini
[Server]
url=http://39.96.41.239:443
downloadToken=你的MSEED_DOWNLOAD_TOKEN

[Consumer]
pollSeconds=5
outputDir=data/mseed
stateFile=data/state/cursor.txt

[DeviceDirs]
; 可选：为每台设备指定确切的保存目录
; 01=D:\mseed\device01
```

相对路径以 `MseedConsumer.exe` 所在目录为基准。程序启动后自动按 `pollSeconds` 轮询，
也可以在界面修改配置并点击“保存”和“启动”。

“mseed消费”页的“设备保存目录”就是该设备的**确切保存目录**。为保证路径有效，该字段为只读，
点击“浏览...”选择电脑上的任意目录。目录选到哪里，mseed 就保存到哪里，不会再自动追加设备编号。
每台设备的路径保存在 `config.ini` 的 `[DeviceDirs]` 段，例如：

```ini
[DeviceDirs]
01=D:\mseed\device01
02=E:\data\device02
```

`[Consumer] outputDir` 只作为没有单独设置设备目录时的默认模板，默认值为 `data/mseed`。
修改保存目录后，停止并重新启动“获取数据”即可生效。

`downloadToken` 同时作为项目控制令牌，用于设备列表、参数下发和状态查询；连接成功后
窗口标题会显示服务端返回的项目名。一个控制台只填写一个项目的地址和令牌，多个项目请使用
不同的 `config.ini` 或安装目录。完整多项目部署见
[`../../docs/Mseed服务器部署与接口协议.md`](../../docs/Mseed服务器部署与接口协议.md) 第十一节。

## Windows 运行

```bat
cd /d D:\MseedConsumer\dist-win64
MseedConsumer.exe
```

不要删除 `stateFile` 指向的 `cursor.txt`，否则程序会从 ID 0 重新消费。业务处理代码应添加在
`MseedConsumer::finishDownload()` 的注释位置，并且只有业务处理成功后才调用 `saveCursor()`。
