#!/bin/sh
# 一键跑完 llm_web v1.3.0 的回归测试
#   1) 起一个测试用 llm_web（默认 8095，工作区 /data/hello）
#   2) 起假模型端点（默认 8099）
#   3) 跑六套测试：/api/fs 冒烟、Files 面板 UI、Chat+工具 UI、工具形态、沙箱范围、超时语义
#   4) 收尾：停掉自己起的进程
#
# 用法: sh run_all.sh [端口]        （默认 8095；想连已经在跑的服务就自己改 LLM_WEB）
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PORT=${1:-8095}
WEB="http://127.0.0.1:$PORT"
WEBPIDF=/tmp/llm_web_test_$PORT.pid
MOCKPIDF=/tmp/llm_web_mock.pid
export LLM_WEB="$WEB"
export MOCK_URL="http://127.0.0.1:8099/v1"
export MOCK_LOG=/tmp/llm_web_mock_requests.jsonl
export FSTEST_DIR=/data/hello/tmp/llm_web_tests
export LLM_WEB_BIN=/tmp/llm_web_test_bin

fail=0

echo "== 0. 编译测试用二进制 =="
# 这块板子编译一次要几十秒，源码没变就不重编
if [ -x /tmp/llm_web_test_bin ] && [ /tmp/llm_web_test_bin -nt "$ROOT/llm_web.cpp" ]; then
    echo "   （源码没变，用现成的 /tmp/llm_web_test_bin）"
else
    g++ -std=c++17 -Wall -Wextra -O2 "$ROOT/llm_web.cpp" -o /tmp/llm_web_test_bin || exit 1
fi

echo "== 1. 起测试服务 $WEB =="
/tmp/llm_web_test_bin --port "$PORT" --sock /tmp/llm_shell.sock --fs-root /data/hello \
    >/tmp/llm_web_test_$PORT.log 2>&1 &
echo $! > "$WEBPIDF"
sleep 0.7
curl -sS "$WEB/api/health" >/dev/null || { echo "服务起不来，看 /tmp/llm_web_test_$PORT.log"; exit 1; }

echo "== 2. 起假模型 8099 =="
python3 "$HERE/mock_llm.py" 8099 >/tmp/llm_web_mock.log 2>&1 &
echo $! > "$MOCKPIDF"
sleep 0.7
if ! kill -0 "$(cat $MOCKPIDF)" 2>/dev/null; then
    echo "假模型起不来（8099 被占？），看 /tmp/llm_web_mock.log"; cat /tmp/llm_web_mock.log; exit 1
fi

echo
echo "########## 1/6  /api/fs 冒烟 ##########"
python3 "$HERE/fs_smoke.py" || fail=1

echo
echo "########## 2/6  Files 面板 UI ##########"
node "$HERE/ui_test.js" || fail=1

echo
echo "########## 3/6  Chat + 工具 UI ##########"
node "$HERE/chat_ui_test.js" || fail=1

echo
echo "########## 4/6  工具调用形态兼容 ##########"
python3 "$HERE/tool_shapes_test.py" || fail=1

echo
echo "########## 5/6  沙箱范围（默认 / / --fs-root / --fs-allow-outside） ##########"
python3 "$HERE/fs_scope_test.py" || fail=1

echo
echo "########## 6/6  Chat「超时(s)」语义（夹紧 / 生效 / 报超时） ##########"
python3 "$HERE/chat_timeout_test.py" || fail=1

echo
echo "== 收尾 =="
kill "$(cat $WEBPIDF)" 2>/dev/null; rm -f "$WEBPIDF"
kill "$(cat $MOCKPIDF)" 2>/dev/null; rm -f "$MOCKPIDF"
rm -rf "$FSTEST_DIR"
[ "$fail" = 0 ] && echo "全部通过 ✔" || echo "有失败项 ✘"
exit $fail
