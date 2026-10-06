#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""沙箱范围测试（v1.3.0 起）

验五件事：
  A. 默认启动（不带 --fs-root）→ 根是 /，也就是"可以越界"，工作区外的路径也能操作
  B. 收窄 --fs-root /data/hello  → 越界被拒，且提示就是那句文案
  C. --fs-root /data/hello --fs-allow-outside → 越界检查关掉
  D. 勾选框（请求级 allow_outside，v1.3.3）能临时放开/收紧，连模型的 fs_* 工具一起管
     —— B/C 里就有请求级的用例，D 段再拿假模型跑一遍 /api/chat
  E. --no-fork 下范围也是严格「按请求」（v1.3.5）：上一次请求的 root / allow_outside
     不许残留到下一笔（fork 模式本来就干净，这里专门盯串行模式）

自己起几个不同参数的 llm_web（8101~8106），不碰正式服务。
用法: python3 fs_scope_test.py      （二进制默认 /tmp/llm_web_test_bin，可用 LLM_WEB_BIN 指定）
"""
import json, os, re, shutil, subprocess, sys, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BIN  = os.environ.get("LLM_WEB_BIN", "/tmp/llm_web_test_bin")
NODE = os.environ.get("NODE", "node")

DIR = "/data/hello/tmp/llm_web_scope_tests"   # 在 /data/hello 里面
OUT = "/tmp/llm_web_scope_outside"            # 在 /data/hello 外面、但在 / 里面
WANT = "越界：只允许操作 /data/hello 内的路径（要放开就加 --fs-allow-outside）"

ok = fail = 0
def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1; print("  PASS  %s" % name)
    else:
        fail += 1; print("  FAIL  %s  %s" % (name, extra))

def post_to(url, payload):
    req = urllib.request.Request(url, data=json.dumps(payload).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req).read().decode("utf-8"))

def post(base, payload):
    return post_to(base + "/api/fs", payload)

def getjson(url):
    return json.loads(urllib.request.urlopen(url, timeout=5).read().decode("utf-8"))

class Srv:
    """起一个 llm_web，退出时收掉"""
    def __init__(self, port, extra):
        self.port = port
        self.args = [BIN, "--port", str(port), "--sock", "/tmp/llm_shell.sock"] + extra
        self.log  = "/tmp/llm_web_scope_%d.log" % port
    def __enter__(self):
        self.f = open(self.log, "w")
        self.p = subprocess.Popen(self.args, stdout=self.f, stderr=subprocess.STDOUT)
        self.base = "http://127.0.0.1:%d" % self.port
        for _ in range(60):
            if self.p.poll() is not None:
                raise RuntimeError("进程自己退了，看 " + self.log)
            try:
                getjson(self.base + "/api/health"); return self
            except Exception:
                time.sleep(0.2)
        raise RuntimeError("起不来，看 " + self.log)
    def __exit__(self, *a):
        self.p.terminate()
        try: self.p.wait(timeout=5)
        except Exception: self.p.kill()
        self.f.close()

def ui_check(base, allow, label):
    """跑前端勾选框测试（node + domstub），把子进程的通过/失败数并进来"""
    global ok, fail
    if not shutil.which(NODE):
        print("  SKIP  %s（没装 node）" % label); return
    env = dict(os.environ, LLM_WEB=base, FS_ALLOW="1" if allow else "0")
    p = subprocess.run([NODE, os.path.join(HERE, "scope_ui_test.js")],
                       env=env, capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    if p.stderr.strip():
        sys.stdout.write(p.stderr)
    m = re.search(r"结果: (\d+) 通过 / (\d+) 失败", p.stdout)
    if m:
        ok += int(m.group(1)); fail += int(m.group(2))
    else:
        check("前端勾选框：%s" % label, False, "没能解析出结果（exit=%d）" % p.returncode)

if not os.path.exists(BIN):
    print("找不到二进制 %s（先跑 sh tests/run_all.sh 编一个，或用 LLM_WEB_BIN 指定）" % BIN)
    sys.exit(2)

shutil.rmtree(DIR, ignore_errors=True)
shutil.rmtree(OUT, ignore_errors=True)

# ---------------- A. 默认：根是 /，可以越界 ----------------
with Srv(8101, []) as s:
    print("== A. 默认启动（不带 --fs-root）：根 = / ，可以越界 ==")
    h = getjson(s.base + "/api/health")
    check("health 报 fs_root=/",           h.get("fs_root") == "/", h)
    check("health 报 fs_allow_outside=false", h.get("fs_allow_outside") is False, h)

    r = post(s.base, {"action": "list", "path": "/"})
    check("列 / 不再算越界", r.get("ok") is True and r.get("count", 0) > 0, r.get("error"))
    check("响应里带 fs_root / fs_allow_outside",
          r.get("fs_root") == "/" and r.get("fs_allow_outside") is False, r)

    r = post(s.base, {"action": "read", "path": "/etc/hostname"})
    check("读工作区外的 /etc/hostname 成功",
          r.get("ok") is True and r.get("content", "").strip() != "", r.get("error"))

    r = post(s.base, {"action": "stat", "path": "/"})
    check("stat / 成功", r.get("ok") is True and r.get("is_dir") is True, r.get("error"))

    r = post(s.base, {"action": "mkdir", "path": DIR})
    check("mkdir 测试目录", r.get("ok") is True, r.get("error"))
    r = post(s.base, {"action": "write", "path": OUT + "/a_default.txt", "content": "outside-ok\n", "mkdirs": True})
    check("往 /data/hello 外面写文件成功", r.get("ok") is True, r.get("error"))
    r = post(s.base, {"action": "read", "path": OUT + "/a_default.txt"})
    check("读回来", r.get("content") == "outside-ok\n", r.get("error"))

    r = post(s.base, {"action": "write", "path": "tmp/llm_web_scope_tests/rel.txt",
                      "content": "rel\n", "mkdirs": True})
    check("相对路径按 / 解析",
          r.get("ok") is True and r.get("path") == "/tmp/llm_web_scope_tests/rel.txt", r)

    r = post(s.base, {"action": "read", "path": "/data/hello/os/shell/../../../../etc/hostname"})
    check(".. 穿越在 / 沙箱下也合法", r.get("ok") is True, r.get("error"))

    r = post(s.base, {"action": "scan", "path": "/data/hello/os/file"})
    check("scan 工作区外目录可用", r.get("ok") is True, r.get("error"))

    ui_check(s.base, False, "默认（收紧，勾着）")

# ---------------- B. 收窄到 /data/hello：越界被拒 ----------------
with Srv(8102, ["--fs-root", "/data/hello"]) as s:
    print("== B. --fs-root /data/hello：越界被拒，文案对上 ==")
    h = getjson(s.base + "/api/health")
    check("health 报 fs_root=/data/hello", h.get("fs_root") == "/data/hello", h)
    check("health 报 fs_allow_outside=false", h.get("fs_allow_outside") is False, h)

    r = post(s.base, {"action": "list", "path": "/etc"})
    check("列 /etc 被拒", r.get("ok") is False, r)
    check("错误就是那句文案", r.get("error") == WANT, repr(r.get("error")))

    r = post(s.base, {"action": "write", "path": OUT + "/b.txt", "content": "x", "mkdirs": True})
    check("越界写被拒", r.get("ok") is False and r.get("error") == WANT, r)

    r = post(s.base, {"action": "read", "path": "/data/hello/os/shell/../../../../etc/passwd"})
    check(".. 穿越被拒", r.get("ok") is False, r)

    r = post(s.base, {"action": "list", "path": "/"})
    check("列 / 被拒（/ 是工作区的上级）", r.get("ok") is False, r.get("error"))

    r = post(s.base, {"action": "list", "path": DIR})
    check("工作区内的路径照常可用", r.get("ok") is True, r.get("error"))

    # v1.3.3：请求级 allow_outside —— 网页 Files 面板那个勾选框就是跟着每个请求带它
    r = post(s.base, {"action": "list", "path": "/etc", "allow_outside": True})
    check("请求带 allow_outside:true → 这次请求临时放开",
          r.get("ok") is True and r.get("fs_allow_outside") is True, r)
    r = post(s.base, {"action": "list", "path": "/etc", "allow_outside": False})
    check("请求带 allow_outside:false → 越界照旧被拒",
          r.get("ok") is False and r.get("error") == WANT, r.get("error"))
    r = post(s.base, {"action": "list", "path": "/etc", "allow_outside": "0"})
    check('allow_outside 给字符串 "0"（GET 那种形态）也认',
          r.get("ok") is False and r.get("error") == WANT, r.get("error"))
    r = getjson(s.base + "/api/fs?action=list&path=/etc&allow_outside=1")
    check("GET /api/fs?...&allow_outside=1 也放行", r.get("ok") is True, r.get("error"))
    r = post(s.base, {"action": "list", "path": "/etc", "allow_outside": "maybe"})
    check("allow_outside 非法值报错（不是默默放行）",
          r.get("ok") is False and "allow_outside 非法" in (r.get("error") or ""), r.get("error"))

    # 勾选框也管着 Chat 那条路（/api/chat 用的是同一套解析）
    r = post_to(s.base + "/api/chat", {"model": "m", "api_key": "k",
                                       "base_url": "http://127.0.0.1:1/v1",
                                       "messages": "[]", "tools": {"fs": True},
                                       "allow_outside": "maybe"})
    check("/api/chat 也认 allow_outside（非法值报错）",
          r.get("ok") is False and "allow_outside 非法" in (r.get("error") or ""), r)

# ---------------- C. --fs-allow-outside：彻底放开 ----------------
with Srv(8103, ["--fs-root", "/data/hello", "--fs-allow-outside"]) as s:
    print("== C. --fs-allow-outside：越界检查关掉 ==")
    h = getjson(s.base + "/api/health")
    check("health 报 fs_allow_outside=true", h.get("fs_allow_outside") is True, h)
    check("fs_root 仍是 /data/hello", h.get("fs_root") == "/data/hello", h)

    r = post(s.base, {"action": "list", "path": "/etc"})
    check("列 /etc 放行", r.get("ok") is True, r.get("error"))

    r = post(s.base, {"action": "write", "path": OUT + "/c.txt", "content": "ok\n", "mkdirs": True})
    check("越界写放行", r.get("ok") is True, r.get("error"))
    r = post(s.base, {"action": "read", "path": OUT + "/c.txt"})
    check("读回来", r.get("ok") is True and "ok" in r.get("content", ""), r.get("error"))

    r = post(s.base, {"action": "remove", "path": OUT + "/c.txt"})
    check("越界删放行", r.get("ok") is True, r.get("error"))

    r = post(s.base, {"action": "list", "path": "/etc", "root": "/etc"})
    check("请求级 root 覆盖仍然可用",
          r.get("ok") is True and r.get("fs_root") == "/etc", r.get("error"))

    # v1.3.3：服务端虽然放开了，勾选框（请求级）还能重新收紧
    r = post(s.base, {"action": "list", "path": "/etc", "allow_outside": False})
    check("服务端用 --fs-allow-outside 放开了，请求带 false 仍能收紧",
          r.get("ok") is False and r.get("error") == WANT and r.get("fs_allow_outside") is False,
          r.get("error"))
    r = post(s.base, {"action": "list", "path": "/etc", "allow_outside": True})
    check("再带 allow_outside:true 又放开", r.get("ok") is True, r.get("error"))

    ui_check(s.base, True, "放开（不勾）")

# ---------------- E. --no-fork：范围严格「按请求」，上一笔的残留不许带进来 ----------------
#   v1.3.5 修的就是这里：--no-fork 是同一个进程连着处理所有连接，而 root / allow_outside
#   是「这一次请求」的语义。修复前上一次请求带的范围会残留 —— 最坏的情况是上一次带了
#   allow_outside:true，后面没带（或者带着 root 想收紧）的请求会白捡一个放开状态，
#   本该拒的路径就没拒。默认 fork-per-connection 时不会有这问题，所以两种都要验。
with Srv(8105, ["--fs-root", "/data/hello", "--no-fork"]) as s:
    print("== E. --no-fork：范围严格按请求（v1.3.5）==")
    r = post(s.base, {"action": "list", "path": "/etc", "allow_outside": True})
    check("no-fork：带 allow_outside:true 的那一次照常放开",
          r.get("ok") is True and r.get("fs_allow_outside") is True, r.get("error"))

    r = post(s.base, {"action": "list", "path": "/etc"})
    check("no-fork：紧接着不带参数的请求照旧被拒（放开状态不许残留）",
          r.get("ok") is False and r.get("error") == WANT, r)
    check("no-fork：这条请求回包里的 fs_allow_outside 回到启动默认 false",
          r.get("fs_allow_outside") is False, r)

with Srv(8106, ["--fs-root", "/", "--no-fork"]) as s:
    r = post(s.base, {"action": "list", "path": "/data/hello", "root": "/data/hello"})
    check("no-fork：请求级 root 那一次生效", r.get("fs_root") == "/data/hello", r.get("error"))

    r = post(s.base, {"action": "list", "path": "/etc"})
    check("no-fork：下一条不带 root 的请求回到启动根 /（root 不许残留）",
          r.get("ok") is True and r.get("fs_root") == "/", r)

    post(s.base, {"action": "list", "path": "/etc", "allow_outside": True})
    r = post(s.base, {"action": "list", "path": "/etc", "root": "/data/hello"})
    check("no-fork：上一笔的 allow_outside 不许把这一笔的 root 限制冲掉（收紧仍被拒）",
          r.get("ok") is False and r.get("error") == WANT, r)

# ---------------- D. 模型工具也吃 allow_outside（勾选框管到 /api/chat） ----
#   mock_llm.py 的 sk-test 人格会要一笔 fs_write，写到 FSTEST_DIR/from_chat.txt；
#   这里把沙箱根收成一个跟它不相干的目录，于是这笔写入算不算越界、能不能落盘，
#   就完全取决于请求里的 allow_outside（= 网页上那个勾选框）了。
MOCK   = os.environ.get("MOCK_URL", "http://127.0.0.1:8099/v1")
FSTEST = os.environ.get("FSTEST_DIR", "/data/hello/tmp/llm_web_tests")
TARGET = FSTEST + "/from_chat.txt"

def mock_alive():
    try:
        urllib.request.urlopen(MOCK + "/models", timeout=2)
        return True
    except urllib.error.HTTPError:
        return True          # 假模型只实现了 POST，回 501 也算活着
    except Exception:
        return False

print("== D. 勾选框也管着模型的工具（/api/chat 带 allow_outside）==")
if not mock_alive():
    print("  SKIP  假模型 %s 没起（python3 tests/mock_llm.py 8099 &）" % MOCK)
else:
    with Srv(8104, ["--fs-root", "/tmp/llm_web_scope_root"]) as s:
        os.makedirs(FSTEST, exist_ok=True)
        try: os.remove(TARGET)
        except OSError: pass

        def chat(outside):
            return post_to(s.base + "/api/chat", {
                "model": "mock-1", "api_key": "sk-test", "base_url": MOCK,
                "messages": json.dumps([{"role": "user", "content": "帮我建个文件"}]),
                "tools": {"fs": True}, "max_rounds": 3, "timeout": 30,
                "allow_outside": outside})

        r = chat(False)
        log = [t for t in (r.get("tool_log") or []) if t.get("tool") == "fs_write"]
        check("收紧时模型那笔 fs_write 被拒（越界）",
              bool(log) and log[0].get("ok") is False and "越界" in (log[0].get("result") or ""),
              json.dumps(r.get("tool_log"), ensure_ascii=False)[:300])
        check("收紧时磁盘上确实没这个文件", not os.path.exists(TARGET))

        r = chat(True)
        log = [t for t in (r.get("tool_log") or []) if t.get("tool") == "fs_write"]
        check("放开后同一笔 fs_write 成功",
              bool(log) and log[0].get("ok") is not False,
              json.dumps(r.get("tool_log"), ensure_ascii=False)[:300])
        check("放开后文件真的落盘", os.path.exists(TARGET))
        try: os.remove(TARGET)
        except OSError: pass

shutil.rmtree(DIR, ignore_errors=True)
shutil.rmtree(OUT, ignore_errors=True)

print("\n结果: %d 通过 / %d 失败" % (ok, fail))
sys.exit(1 if fail else 0)
