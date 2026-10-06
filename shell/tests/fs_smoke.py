#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llm_web v1.2.0 /api/fs 冒烟测试"""
import json, urllib.request, os, shutil, sys

BASE    = os.environ.get("LLM_WEB", "http://127.0.0.1:8095")
SANDBOX = os.environ.get("FSTEST_DIR", "/data/hello/tmp/llm_web_tests")

def api(payload):
    req = urllib.request.Request(BASE + "/api/fs",
                                 data=json.dumps(payload).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req).read().decode("utf-8"))

def get(qs):
    return json.loads(urllib.request.urlopen(BASE + "/api/fs?" + qs).read().decode("utf-8"))

ok = fail = 0
def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1; print("  PASS  %s" % name)
    else:
        fail += 1; print("  FAIL  %s  %s" % (name, extra))

shutil.rmtree(SANDBOX, ignore_errors=True)
print("== 1. mkdir / write / read ==")
r = api({"action":"mkdir","path":SANDBOX})
check("mkdir", r.get("ok") is True, r)
r = api({"action":"write","path":SANDBOX+"/a.txt","content":"第一行\n第二行\n"})
check("write utf-8", r.get("ok") is True and r.get("bytes",0) > 0, r)
r = api({"action":"read","path":SANDBOX+"/a.txt"})
check("read back", r.get("content") == "第一行\n第二行\n" and r.get("encoding")=="utf-8", r)
check("read size", r.get("size") == len("第一行\n第二行\n".encode()), r.get("size"))

print("== 2. append / 相对路径 / mkdirs ==")
api({"action":"append","path":SANDBOX+"/a.txt","content":"third\n"})
r = api({"action":"read","path":SANDBOX+"/a.txt","charset":"utf-8"})
check("append", r["content"].endswith("third\n"), r)
r = api({"action":"write","path":"tmp/llm_web_tests/sub/deep/b.txt","content":"x","mkdirs":True})
check("相对路径+自动建目录", r.get("ok") is True and r["path"].endswith("/sub/deep/b.txt"), r)

print("== 3. GBK 读 / 写 ==")
r = api({"action":"write","path":SANDBOX+"/gbk.txt","content":"中文内容\n","charset":"gbk"})
check("write gbk", r.get("ok") is True and r.get("charset")=="gbk", r)
raw = open(SANDBOX+"/gbk.txt","rb").read()
check("磁盘上真的是 GBK 字节", raw == b"\xd6\xd0\xce\xc4\xc4\xda\xc8\xdd\r?\n" or raw == "中文内容\n".encode("gbk"), raw)
r = api({"action":"read","path":SANDBOX+"/gbk.txt"})
check("GBK 自动识别回 UTF-8", r.get("content")=="中文内容\n" and r.get("encoding")=="gbk", r)

print("== 4. CRLF / 二进制 ==")
api({"action":"write","path":SANDBOX+"/crlf.txt","content":"a\nb\n","eol":"crlf"})
check("CRLF 落盘", open(SANDBOX+"/crlf.txt","rb").read() == b"a\r\nb\r\n")
open(SANDBOX+"/bin.dat","wb").write(b"\x00\x01\x02\xff")
r = api({"action":"read","path":SANDBOX+"/bin.dat"})
check("二进制返回 base64", r.get("encoding")=="binary" and r.get("base64")=="AAEC/w==", r)

print("== 5. list / stat / scan ==")
r = api({"action":"list","path":SANDBOX})
names = [e["name"] for e in r["entries"]]
check("list 目录优先排序", names[0] == "sub" and "a.txt" in names, names)
check("list 带 size/mtime", "mtime" in r["entries"][-1], r["entries"][-1])
r = api({"action":"stat","path":SANDBOX+"/a.txt"})
check("stat", r.get("is_regular") is True and r.get("size",0) > 0, r)
r = api({"action":"scan","path":SANDBOX,"ext":".txt"})
check("scan ext 过滤", "a.txt" in r["tree"] and "bin.dat" not in r["tree"], r["tree"][:120])
check("scan stats", r["stats"]["files"] >= 4 and r["stats"]["matched"] == 4, r["stats"])

print("== 6. copy / move / remove ==")
check("copy", api({"action":"copy","path":SANDBOX+"/a.txt","to":SANDBOX+"/a2.txt"}).get("ok") is True)
check("copy 不覆盖已存在", api({"action":"copy","path":SANDBOX+"/a.txt","to":SANDBOX+"/a2.txt"}).get("ok") is False)
check("move", api({"action":"move","path":SANDBOX+"/a2.txt","to":SANDBOX+"/a3.txt"}).get("ok") is True)
check("remove 文件", api({"action":"remove","path":SANDBOX+"/a3.txt"}).get("ok") is True)
check("remove 目录需 recursive", api({"action":"remove","path":SANDBOX+"/sub"}).get("ok") is False)
check("remove 目录 recursive", api({"action":"remove","path":SANDBOX+"/sub","recursive":True}).get("ok") is True)

print("== 7. gen 模板 ==")
api({"action":"write","path":SANDBOX+"/tpl.txt",
     "content":"// {{__FILENAME__}} {{NAME}} {{__YEAR__}}\n// 缺的: {{MISSING}}\n"})
r = api({"action":"gen","template":SANDBOX+"/tpl.txt","output":SANDBOX+"/out.cpp",
         "vars":{"NAME":"llm_shell"}})
check("gen 写成功", r.get("ok") is True and r.get("action")=="gen", r)
c = api({"action":"read","path":SANDBOX+"/out.cpp"})["content"]
check("gen 内置变量+自定义变量", "out.cpp" in c and "llm_shell" in c and "{{MISSING}}" in c, c)

print("== 8. 沙箱 ==")
r = api({"action":"list","path":"/etc"})
check("越界被拒", r.get("ok") is False and "越界" in r.get("error",""), r)
r = api({"action":"write","path":"/tmp/evil.txt","content":"x"})
check("越界写被拒", r.get("ok") is False, r)
r = api({"action":"read","path":"/data/hello/os/shell/../../../../etc/passwd"})
check(".. 穿越被拒", r.get("ok") is False, r)
r = api({"action":"list","path":"/data/hello/os","root":"/data/hello/os"})
check("root 可覆盖", r.get("ok") is True and r.get("fs_root")=="/data/hello/os", r)
r = api({"action":"list","path":"/data/hello/tmp","root":"/data/hello/os"})
check("换 root 后旧路径被拒", r.get("ok") is False, r)

print("== 9. GET 形式 / 错误处理 ==")
r = get("action=list&path=/data/hello/os/file")
check("GET list", r.get("ok") is True and r.get("count")==4, r.get("count"))
r = get("action=nonsense&path=/data/hello")
check("未知 action", r.get("ok") is False, r)
r = api({"action":"read","path":SANDBOX+"/nope.txt"})
check("读不存在", r.get("ok") is False and "不存在" in r["error"], r)
r = api({"action":"write","path":SANDBOX+"/a.txt"})
check("write 缺 content", r.get("ok") is False, r)
r = api({"action":"list"})
check("list 缺 path 默认化", isinstance(r.get("ok"), bool), r)

print("\n结果: %d 通过 / %d 失败" % (ok, fail))
shutil.rmtree(SANDBOX, ignore_errors=True)
sys.exit(1 if fail else 0)
