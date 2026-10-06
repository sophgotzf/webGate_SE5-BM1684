#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""假的 OpenAI 兼容端点：验证 llm_web 的 function calling 循环。
用 Authorization: Bearer sk-xxx 切换行为：
  sk-test   : 第1轮 fs_write + fs_list，第2轮给最终答复
  sk-shell  : 第1轮 run_shell，第2轮给最终答复
  sk-loop   : 一直要工具（每次内容不同），用来试 max_rounds
  sk-dup    : 一直要同一个工具同一个参数，用来试死循环保护
  sk-objargs: 第1轮要 fs_write，但 arguments 直接给成 JSON 对象（不规范网关）
  sk-legacy : 第1轮用老式 function_call 要 fs_write，第2轮 content 给成分片数组
收到的请求 append 到 /data/tmp/webtest/mock_requests.jsonl
"""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG", "/tmp/llm_web_mock_requests.jsonl")
FSTEST = os.environ.get("FSTEST_DIR", "/data/hello/tmp/llm_web_tests")
TARGET = FSTEST + "/from_chat.txt"
LOOPFILE = FSTEST + "/loop.txt"
OBJFILE = FSTEST + "/objargs.txt"
LEGACYFILE = FSTEST + "/legacy.txt"
_counter = {"loop": 0}

def tool_call(cid, name, args):
    return {"id": cid, "type": "function",
            "function": {"name": name, "arguments": json.dumps(args, ensure_ascii=False)}}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("utf-8")
        try:
            req = json.loads(body)
        except Exception as e:
            req = {"_parse_error": str(e), "_raw": body[:500]}
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(json.dumps({"path": self.path, "auth": self.headers.get("Authorization"),
                                "req": req}, ensure_ascii=False) + "\n")

        mode = (self.headers.get("Authorization") or "").replace("Bearer ", "").strip()
        msgs = req.get("messages") or []
        tool_results = [m.get("content", "") for m in msgs if m.get("role") == "tool"]
        has_tool_result = bool(tool_results)
        tool_txt = "".join(tool_results).replace(" ", "")

        if mode == "sk-shell" and not has_tool_result:
            msg = {"role": "assistant", "content": None,
                   "tool_calls": [tool_call("call_s1", "run_shell",
                                            {"command": "echo tool-shell-ok && pwd", "timeout": 5})]}
        elif mode == "sk-loop":
            _counter["loop"] += 1
            msg = {"role": "assistant", "content": None,
                   "tool_calls": [tool_call("call_loop%d" % _counter["loop"], "fs_write",
                                            {"path": LOOPFILE,
                                             "content": "round %d\n" % _counter["loop"]})]}
        elif mode == "sk-dup" and "死循环" not in tool_txt:
            msg = {"role": "assistant", "content": None,
                   "tool_calls": [tool_call("call_d1", "fs_list", {"path": "/data/hello/os"})]}
        elif mode == "sk-objargs" and not has_tool_result:
            # 不规范的网关：arguments 是 JSON 对象，而不是"字符串里套 JSON"
            msg = {"role": "assistant", "content": None,
                   "tool_calls": [{"id": "call_o1", "type": "function",
                                   "function": {"name": "fs_write",
                                                "arguments": {"path": OBJFILE,
                                                              "content": "arguments 是对象也要能落盘\n",
                                                              "eol": "crlf"}}}]}
        elif mode == "sk-legacy" and not has_tool_result:
            # 老式 function_call（单个、非 tool_calls）
            msg = {"role": "assistant", "content": None,
                   "function_call": {"name": "fs_write",
                                     "arguments": json.dumps({"path": LEGACYFILE,
                                                              "content": "老式 function_call 也要能落盘\n"},
                                                             ensure_ascii=False)}}
        elif mode == "sk-legacy":
            # 有些网关把最终答复的 content 给成"分片数组"
            msg = {"role": "assistant",
                   "content": [{"type": "text", "text": "分片内容也要能读出来"},
                               {"type": "text", "text": "（第二片）"}]}
        elif mode == "sk-test" and not has_tool_result:
            msg = {"role": "assistant", "content": None,
                   "tool_calls": [
                       tool_call("call_w1", "fs_write",
                                 {"path": TARGET,
                                  "content": "由 chat 工具创建\n第二行\n", "eol": "crlf"}),
                       tool_call("call_l1", "fs_list", {"path": "/data/hello/os/file"})]}
        else:
            ok = '"ok":true' in tool_txt
            msg = {"role": "assistant",
                   "content": "工具调用完成，看到 ok=%s。" % ok}

        out = json.dumps({"id": "chatcmpl-mock", "object": "chat.completion",
                          "model": req.get("model", "mock"),
                          "choices": [{"index": 0, "message": msg,
                                       "finish_reason": "stop"}]},
                         ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else int(os.environ.get("MOCK_PORT", "8099"))
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
