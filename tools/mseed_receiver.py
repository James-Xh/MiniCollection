# -*- coding: utf-8 -*-
"""
mseed_receiver.py — mseed 接收服务（测试/部署两用）

配合 MiniCollection 的 MseedUploader 使用，实现 mseed 文件上传与断点续传。

协议（与 MseedUploader 约定一致）：
  HEAD /coalmine/mseed/upload?name=<文件名>
      返回 200 + 响应头 X-Uploaded-Bytes: <服务端已接收字节数>；
      文件不存在时返回 X-Uploaded-Bytes: 0。
  PUT  /coalmine/mseed/upload?name=<文件名>
      请求头 Content-Range: bytes <start>-<end>/<total>
      每个分片返回 2xx；接收完最后一片（累计字节数 == total）即写盘完成。

实现说明：
  - 分片数据实时追加写入 <保存目录>/<文件名>.part；
    服务端已接收字节数 = .part 文件的实际大小（权威值）。
  - 收到最后一片后将 .part 原子重命名为正式文件名，保证不会读到半截文件。
  - 仅依赖 Python 标准库，无需安装任何第三方包。
  - 服务端崩溃/重启后，.part 文件仍在磁盘上，客户端重新 HEAD 即可从断点续传。

用法（局域网测试机 / 云服务器通用）：
  python mseed_receiver.py [端口] [保存目录]
  例：python mseed_receiver.py 9002 D:/mseed_data
  客户端界面里服务器地址填 http://<本机IP>:9002

云服务器部署（Linux systemd 示例）见文件末尾注释。
"""

import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs, unquote

# 可通过命令行覆盖：端口、保存目录
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9002
SAVE_DIR = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else "./mseed_data"

# 文件名只允许字母数字下划线点横线，防止路径穿越
SAFE_NAME = re.compile(r"^[A-Za-z0-9._-]+$")

os.makedirs(SAVE_DIR, exist_ok=True)


def log(msg):
    print("[%s] %s" % (_now(), msg), flush=True)


def _now():
    import datetime
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


class MseedUploadHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"   # 支持 keep-alive，分片连续上传更快

    def _file_name(self):
        """从 URL query 提取并校验文件名，非法返回 None。"""
        query = parse_qs(urlsplit(self.path).query)
        name = query.get("name", [""])[0]
        if not name or not SAFE_NAME.match(name):
            return None
        return name

    def _respond(self, code, extra_headers=None):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        for k, v in (extra_headers or {}).items():
            self.send_header(k, v)
        self.end_headers()

    # ---------- HEAD：查询服务端已接收字节数（断点续传依据） ----------
    def do_HEAD(self):
        name = self._file_name()
        if name is None:
            self._respond(400)
            return
        part_path = os.path.join(SAVE_DIR, name + ".part")
        final_path = os.path.join(SAVE_DIR, name)
        if os.path.exists(final_path):
            received = os.path.getsize(final_path)   # 已完成，按完整大小返回
        elif os.path.exists(part_path):
            received = os.path.getsize(part_path)
        else:
            received = 0
        log("HEAD %s -> 已接收 %d 字节" % (name, received))
        self._respond(200, {"X-Uploaded-Bytes": str(received)})

    # ---------- PUT：接收一个分片 ----------
    def do_PUT(self):
        name = self._file_name()
        if name is None:
            self._respond(400)
            return

        # 解析 Content-Range: bytes <start>-<end>/<total>
        cr = self.headers.get("Content-Range", "")
        m = re.match(r"bytes\s+(\d+)-(\d+)/(\d+)", cr)
        if not m:
            log("PUT %s 缺少/非法 Content-Range" % name)
            self._respond(400)
            return
        start, end, total = int(m.group(1)), int(m.group(2)), int(m.group(3))

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""
        if len(body) != end - start + 1:
            log("PUT %s 数据长度不匹配：Content-Length=%d 期望=%d"
                % (name, len(body), end - start + 1))
            self._respond(400)
            return

        part_path = os.path.join(SAVE_DIR, name + ".part")
        # 客户端从 start 位置续传：截掉 start 之后的内容再追加（防乱序/重复分片）
        mode = "r+b" if os.path.exists(part_path) else "wb"
        with open(part_path, mode) as f:
            f.truncate(start)
            f.seek(start)
            f.write(body)

        received = start + len(body)
        if received >= total:
            # 最后一片：原子重命名为正式文件
            final_path = os.path.join(SAVE_DIR, name)
            os.replace(part_path, final_path)
            log("PUT %s 上传完成 (%d 字节)" % (name, total))
        else:
            log("PUT %s 分片 %d-%d/%d OK" % (name, start, end, total))
        self._respond(200, {"X-Uploaded-Bytes": str(received)})

    def log_message(self, fmt, *args):
        pass   # 关闭 BaseHTTPRequestHandler 默认逐行访问日志（用自定义 log）


def main():
    server = ThreadingHTTPServer(("0.0.0.0", PORT), MseedUploadHandler)
    log("mseed 接收服务已启动: 端口=%d 保存目录=%s" % (PORT, SAVE_DIR))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("收到退出信号，服务关闭")
        server.server_close()


# ---------------- 云服务器 systemd 部署示例（Linux） ----------------
# 1) 上传本脚本到 /opt/mseed/mseed_receiver.py
# 2) 创建服务 /etc/systemd/system/mseed-receiver.service：
#    [Unit]
#    Description=Mseed Upload Receiver
#    After=network.target
#    [Service]
#    ExecStart=/usr/bin/python3 /opt/mseed/mseed_receiver.py 9002 /data/mseed
#    Restart=always
#    RestartSec=3
#    [Install]
#    WantedBy=multi-user.target
# 3) systemctl daemon-reload && systemctl enable --now mseed-receiver
# 4) 防火墙/安全组放行 9002 端口；
#    客户端服务器地址填 http://<服务器公网IP>:9002
# 注意：公网部署建议在前面加 nginx 反代 + 访问限制（IP 白名单/Token）。

if __name__ == "__main__":
    main()
