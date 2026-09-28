# -*- coding: utf-8 -*-
"""test_upload.py — 模拟 MseedUploader 协议，验证接收服务（含断点续传）。"""
import http.client
import hashlib
import os
import sys

HOST, PORT = "127.0.0.1", 9002
PATH = "/coalmine/mseed/upload?name=test_event.mseed"

# 生成测试数据（3.5 个 256KB 分片）
data = os.urandom(int(256 * 1024 * 3.5))
total = len(data)
CHUNK = 256 * 1024


def head():
    c = http.client.HTTPConnection(HOST, PORT, timeout=5)
    c.request("HEAD", PATH)
    r = c.getresponse()
    recv = int(r.getheader("X-Uploaded-Bytes", "0"))
    r.read(); c.close()
    return recv


def put(start):
    end = min(start + CHUNK, total) - 1
    c = http.client.HTTPConnection(HOST, PORT, timeout=5)
    c.request("PUT", PATH, body=data[start:end + 1], headers={
        "Content-Type": "application/octet-stream",
        "Content-Range": "bytes %d-%d/%d" % (start, end, total),
    })
    r = c.getresponse()
    ok = 200 <= r.status < 300
    r.read(); c.close()
    return ok


# 1) 首次 HEAD 应为 0
r0 = head()
print("HEAD 初始 X-Uploaded-Bytes =", r0)
assert r0 == 0, "初始偏移应为0"

# 2) 上传第一个分片
assert put(0), "分片0失败"
print("分片0 OK, HEAD =", head())

# 3) 模拟中断后重启：HEAD 返回断点，从断点续传
off = head()
assert off == CHUNK, "续传偏移应为CHUNK"
while off < total:
    assert put(off), "分片失败 off=%d" % off
    off = head()
    print("续传中, HEAD =", off)

# 4) 校验文件内容一致
local = hashlib.sha256(data).hexdigest()
remote = hashlib.sha256(open(os.path.join(
    sys.argv[1] if len(sys.argv) > 1 else "./mseed_data",
    "test_event.mseed"), "rb").read()).hexdigest()
print("sha256 local =", local)
print("sha256 remote=", remote)
assert local == remote, "文件内容不一致！"
print("全部测试通过：分片上传 + 断点续传 + 内容校验 OK")
