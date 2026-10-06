#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""llm_web v1.2.1 —— 工具调用形态兼容测试

验证"file 功能能被 web 对话捕获调用"这件事，在各种不规范网关下也成立：
  1) arguments 给成 JSON 对象（而不是字符串里套 JSON）→ 仍要真正落盘
  2) 老式 function_call（不是 tool_calls）          → 仍要真正落盘
  3) 最终答复 content 是分片数组                    → 仍要能读出来
需要：被测服务（默认 8095）+ tests/mock_llm.py（默认 8099）
用法：python3 tool_shapes_test.py
"""
import json, os, sys, time, urllib.request

BASE  = os.environ.get("LLM_WEB",  "http://127.0.0.1:8095")
MOCK  = os.environ.get("MOCK_URL", "http://127.0.0.1:8099/v1")
FSDIR = os.environ.get("FSTEST_DIR", "/data/hello/tmp/llm_web_tests")

OBJFILE    = FSDIR + "/objargs.txt"
LEGACYFILE = FSDIR + "/legacy.txt"

ok = fail = 0


def check(name, cond, extra=""):
    global ok, fail
    if cond:
        ok += 1
        print("  PASS  %s" % name)
    else:
        fail += 1
        print("  FAIL  %s  %s" % (name, extra))


def chat(api_key, text):
    body = {
        "model": "mock-1",
        "api_key": api_key,
        "base_url": MOCK,
        "temperature": 0.2,
        "timeout": 60,
        "messages": json.dumps([{"role": "user", "content": text}], ensure_ascii=False),
        "tools": {"fs": True, "shell": False},
        "root": "/data/hello",
        "max_rounds": 4,
    }
    req = urllib.request.Request(BASE + "/api/chat",
                                 data=json.dumps(body).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=90) as r:
        return json.loads(r.read().decode("utf-8"))


def rm(p):
    try:
        os.unlink(p)
    except OSError:
        pass


os.makedirs(FSDIR, exist_ok=True)

# ---------------------------------------------------------------
print("== 1. arguments 是 JSON 对象（不规范网关） ==")
rm(OBJFILE)
d = chat("sk-objargs", "建个文件")
check("HTTP 通道返回了工具格式", "content" in d and "tool_log" in d, str(d)[:300])
check("整体 ok", d.get("ok") is True, d.get("error"))
check("捕获到 1 次工具调用", len(d.get("tool_log") or []) == 1, d.get("tool_log"))
log = (d.get("tool_log") or [{}])[0]
check("工具名 = fs_write", log.get("tool") == "fs_write", log.get("tool"))
check("执行成功（没报 缺少 path）", log.get("ok") is True and "缺少 path" not in json.dumps(log),
      log.get("error") or log.get("result"))
check("文件真的落盘了（需求核心）", os.path.exists(OBJFILE), OBJFILE)
if os.path.exists(OBJFILE):
    with open(OBJFILE, "rb") as f:
        raw = f.read()
    check("内容正确", raw == "arguments 是对象也要能落盘\r\n".encode("utf-8"), raw[:80])

# ---------------------------------------------------------------
print("== 2. 老式 function_call + 数组 content ==")
rm(LEGACYFILE)
d = chat("sk-legacy", "再建一个")
check("整体 ok", d.get("ok") is True, d.get("error"))
check("捕获到 1 次工具调用", len(d.get("tool_log") or []) == 1, d.get("tool_log"))
log = (d.get("tool_log") or [{}])[0]
check("工具名 = fs_write", log.get("tool") == "fs_write", log.get("tool"))
check("执行成功", log.get("ok") is True, log.get("error") or log.get("result"))
check("文件真的落盘了", os.path.exists(LEGACYFILE), LEGACYFILE)
check("数组 content 被拼成文字", d.get("content") == "分片内容也要能读出来（第二片）",
      repr(d.get("content")))

# ---------------------------------------------------------------
print("== 3. 回灌给模型的历史里，arguments 是合法字符串 ==")
# 第 2 轮请求应该能在假模型日志里看到；这里只做轻量断言：没崩、有回复
check("第二轮也拿到了答复", bool(d.get("content")), repr(d.get("content")))

rm(OBJFILE)
rm(LEGACYFILE)
print("\n结果: %d 通过 / %d 失败" % (ok, fail))
sys.exit(1 if fail else 0)
