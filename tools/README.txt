1. mseed_receiver.py — mseed 接收服务（仅依赖 Python 标准库，无需装任何包）

协议与 MseedUploader 完全对齐：
HEAD /coalmine/mseed/upload?name=<文件名> → 返回 X-Uploaded-Bytes: <已接收字节数>；
PUT + Content-Range: bytes <start>-<end>/<total> → 每片 2xx，收齐最后一片后把 .part 原子重命名为正式文件（不会读到半截文件）；
分片实时写盘到 <保存目录>/<文件名>.part，服务端重启后续传偏移直接取自磁盘上的 .part 大小（天然可靠）；
文件名白名单校验，防路径穿越；
用法：python mseed_receiver.py [端口] [保存目录]，默认 9002、./mseed_data。



2. test_upload.py — 协议测试脚本 模拟客户端完整流程：初始 HEAD → 分片 PUT → 模拟中断重启后从断点续传 → 最后 SHA256 比对本地与远端文件。实测输出：全部测试通过：分片上传 + 断点续传 + 内容校验 OK。

局域网功能测试步骤
在局域网另一台机器上：python mseed_receiver.py 9002 D:/mseed_data（Windows/Linux 均可）；
MiniCollection 界面“mseed网络上传”里填 http://<该机器IP>:9002，勾选启用，点应用；
事件 mseed 生成后即可在接收机器的保存目录看到文件；可随时断网/杀掉客户端再恢复，验证断点续传。