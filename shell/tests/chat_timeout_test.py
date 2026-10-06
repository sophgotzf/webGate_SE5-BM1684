#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llm_web v1.3.2 —— Chat「超时(s)」服务端语义测试

覆盖前端那个输入框背后的契约：
  1) /api/health、/api/tools 把默认值与上限暴露出来（前端的默认值不再各写一份）
  2) 请求里的 timeout 是「一整轮对话的预算」，服务端会夹到 5~300，并回包带出生效值
  3) 不带 timeout 时用服务端默认（--chat-timeout）
  4) 预算真的生效：对着一个「收到请求就装死」的上游，timeout=5 → 5 秒出头就报
     「超时或中断」，而不是干等到天荒地老，更不能变成老 bug 那句 'HTTP 100'
用法：python3 chat_timeout_test.py            （默认连 127.0.0.1:8095）
      LLM_WEB=http://127.0.0.1:8095 python3 chat_timeout_test.py
"""
import json, os, socket, sys, threading, time, urllib.request, urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BASE = os.environ.get("LLM_WEB", "http://127.0.0.1:8095")
SLOW_PORT = int(os.environ.get("SLOW_PORT", "8098"))

ok = fail = 0


def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print("  PASS  %s" % name)
    else:
        fail += 1
        print("  FAIL  %s  %s" % (name, extra))


def get_json(path, timeout=20):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def chat(payload, timeout=120):
    req = urllib.request.Request(BASE + "/api/chat",
                                 data=json.dumps(payload).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8")), time.time() - t0
    except urllib.error.HTTPError as e:
        return json.loads(e.read().decode("utf-8")), time.time() - t0


# ---------------------------------------------------------------- 装死上游
class SlowHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)              # 请求收下来，然后就是不理它
        time.sleep(60)
        try:
            self.send_response(500)
            self.end_headers()
        except Exception:
            pass


def free_port_ok(p):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p))
        return True
    except OSError:
        return False
    finally:
        s.close()


if not free_port_ok(SLOW_PORT):
    print("端口 %d 被占，换个 SLOW_PORT 再跑" % SLOW_PORT)
    sys.exit(2)

srv = ThreadingHTTPServer(("127.0.0.1", SLOW_PORT), SlowHandler)
threading.Thread(target=srv.serve_forever, daemon=True).start()

# ---------------------------------------------------------------
print("== 1. 默认值与上限从服务端拿（/api/health、/api/tools） ==")
h = get_json("/api/health")
check("health 有 chat_timeout", isinstance(h.get("chat_timeout"), int), h)
check("health 有 chat_timeout_max = 300", h.get("chat_timeout_max") == 300, h)
default_to = h.get("chat_timeout")
check("默认值在允许区间内（5~300）", isinstance(default_to, int) and 5 <= default_to <= 300, default_to)
t = get_json("/api/tools")
check("/api/tools 同样有 chat_timeout / chat_timeout_max",
      t.get("chat_timeout") == default_to and t.get("chat_timeout_max") == 300, t)

print("== 2. 请求里的 timeout 被夹到 5~300，并在回包带回来 ==")


def chat_tool(payload, timeout=60):
    payload = dict(payload)
    payload.setdefault("model", "mock-1")
    payload.setdefault("api_key", "sk-test")
    payload.setdefault("base_url", os.environ.get("MOCK_URL", "http://127.0.0.1:8099/v1"))
    payload.setdefault("messages", json.dumps([{"role": "user", "content": "hi"}], ensure_ascii=False))
    payload.setdefault("tools", {"fs": True, "shell": False})
    payload.setdefault("root", "/data/hello")
    payload.setdefault("max_rounds", 2)
    return chat(payload, timeout)


d, _ = chat_tool({"timeout": 7})
check("timeout=7 → 生效值 7", d.get("timeout") == 7, d.get("timeout"))
check("回包带 timeout_max=300", d.get("timeout_max") == 300, d.get("timeout_max"))
d, _ = chat_tool({"timeout": 9999})
check("timeout=9999 → 夹到 300", d.get("timeout") == 300, d.get("timeout"))
d, _ = chat_tool({"timeout": 1})
check("timeout=1 → 夹到 5", d.get("timeout") == 5, d.get("timeout"))
d, _ = chat_tool({"timeout": 0})
check("timeout=0（非法数字）→ 夹到 5", d.get("timeout") == 5, d.get("timeout"))
d, _ = chat_tool({})
check("不带 timeout → 用服务端默认 %s" % default_to, d.get("timeout") == default_to, d.get("timeout"))

print("== 3. 预算真的生效：上游装死，timeout=5 要很快报超时 ==")
d, el = chat_tool({"timeout": 5, "base_url": "http://127.0.0.1:%d/v1" % SLOW_PORT}, timeout=60)
check("整体 ok=false", d.get("ok") is False, str(d)[:200])
check("报的是超时/中断", "超时" in str(d.get("error", "")) or "中断" in str(d.get("error", "")),
      d.get("error"))
check("不再冒充 HTTP 100", str(d.get("error", "")).strip() != "HTTP 100" and d.get("http_code") != 100,
      d.get("error"))
check("耗时 ~5s 而不是干等 60s（实测 %.1fs）" % el, el < 20, "elapsed=%.1fs" % el)
check("错误信息里写明了上限", "5s" in str(d.get("error", "")) or "上限 5" in str(d.get("error", "")),
      d.get("error"))

srv.shutdown()
print("\n结果: %d 通过 / %d 失败" % (ok, fail))
sys.exit(1 if fail else 0)
