# tests —— llm_web v1.3.6 回归测试

一键全跑（自己起测试服务 8095 + 假模型 8099，跑完自己收尾，不碰你正在用的 8093）：

六套用例：`/api/fs` 冒烟 34 + Files 面板 52 + Chat 工具 UI 24 + 工具调用形态兼容 14 + 沙箱范围 75 + 超时语义 15 = 214 项。

```bash
cd /data/hello/os/shell
make test            # 等价 sh tests/run_all.sh
```

预期结尾：`全部通过 ✔`（共 214 项检查）。

## 里面有什么

| 文件 | 说明 |
|---|---|
| `run_all.sh` | 总入口：编译 → 起服务 → 起假模型 → 跑下面六套 → 收尾 |
| `fs_smoke.py` | `/api/fs` 接口冒烟：list/read/write/append/mkdir/remove/move/copy/stat/scan/gen、GBK 读写、CRLF、二进制 base64、沙箱越界、`..` 穿越、GET 形式、错误处理（34 项） |
| `ui_test.js` | 前端 Files 面板（Node 打桩 DOM + 真连服务）：进目录、开文件、改内容保存、新建文件/目录、沙箱勾选框（可手动勾/取消，取消 = 立刻放开、勾上 = 立刻收紧）、工作区 / 时能越界 / 收窄后越界被拒；**v1.3.4：面板里没有删除入口**（每行只剩 4 格、行里没按钮、脚本里不再发 `remove`、刷新后文件还在）、**浏览也被沙箱管**（放开时能打开 `/etc/hostname`，勾上后打开被拒且不往编辑器里塞外面的内容）、**写也一样**（收窄「工作区」后在面板里新建、名字带 `..` 写外面被拒且磁盘上不落盘，工作区里面照常能建）（52 项） |
| `chat_ui_test.js` | 前端 Chat 工具调用全链路：点发送 → 带 tools 的请求 → 模型要 fs_write → 文件真落盘 → 界面轨迹渲染；「超时(s)」输入框默认值/夹紧/存盘/回显；**默认系统提示词逐字比对**（v1.3.6：发给模型的第一段就是 `you are a concise(SE5), helpful assistant. 用中文回答`，且整个请求只有一条 system）（24 项） |
| `tool_shapes_test.py` | 工具调用形态兼容：`arguments` 给成对象、老式 `function_call`、`content` 是分片数组、参数二次解包 —— 都必须真正落盘（14 项） |
| `fs_scope_test.py` | 沙箱范围：默认根 `/` 能越界读写、`--fs-root` 收窄后越界被拒（连提示文案一起比）、`--fs-allow-outside` 关掉检查；请求级 `allow_outside` 能临时放开/收紧、非法值报错，连 `/api/chat` 里模型的 `fs_*` 工具一起验。自己在 8101~8106 起六个不同参数的实例（75 项，含下面那个的前端部分；D 段要假模型在跑，没跑就 SKIP；E 段专门验 `--no-fork` 下范围严格按请求、上一笔的 `root` / `allow_outside` 不许残留，v1.3.5） |
| `scope_ui_test.js` | 只验沙箱勾选框（v1.3.3 起是真开关）：对着一个实例把前端脚本跑起来，看勾选框画成什么样，再手动取消勾选/重新勾上，验它真的放开与收紧（15 项；`FS_ALLOW=0/1` 指定初始状态） |
| `chat_timeout_test.py` | Chat「超时(s)」语义：health/tools 报默认值与上限、越界夹紧、回包带生效值、上游装死时按预算报超时（15 项） |
| `domstub.js` | 极简 DOM 打桩（只实现页面用到的那点 API，不需要 jsdom / 浏览器） |
| `mock_llm.py` | 假的 OpenAI 兼容端点，按 `api_key` 切换行为 |

> 注：v1.3.4 去掉的是**网页 Files 面板里的删除入口**（`ui_test.js` 在验这件事）；
> `/api/fs` 的 `remove` action 本身还在（`fs_smoke.py` 就在验它），Chat 里模型的 `fs_remove` 也照旧。

## 单独跑 / 换地址

```bash
# 用已经在跑的服务（比如正式的 8093）
LLM_WEB=http://127.0.0.1:8093 FSTEST_DIR=/data/hello/tmp/llm_web_tests python3 fs_smoke.py
# ↑ 注意：8093 默认是 --fs-root /，这套用例是按 --fs-root /data/hello 写的，
#   所以「越界被拒 / 越界写被拒 / .. 穿越 / list 目录优先 / scan stats / remove 目录 recursive」
#   这 6 项会红（文件被真的写到 /tmp 去了，正是"根是 /"的正常表现，不是服务有问题）。
#   想全绿：用 make test，或者给被测实例加 --fs-root /data/hello。

# 手动起假模型 + 手动点页面
python3 tests/mock_llm.py 8099 &
# 网页 Chat 里填：模型 mock-1，Key sk-test，Base URL http://127.0.0.1:8099/v1
```

假模型的六种人格（填在 Key 里）：

| api_key | 行为 |
|---|---|
| `sk-test` | 第 1 轮要 `fs_write` + `fs_list`，第 2 轮给最终答复 |
| `sk-shell` | 第 1 轮要 `run_shell`（用来验证命令执行开关） |
| `sk-loop` | 每轮都要写文件，用来验证 `max_rounds` 上限 |
| `sk-dup` | 每轮都要同一个调用，用来验证死循环保护 |
| `sk-objargs` | 第 1 轮要 `fs_write`，但 `arguments` 直接给成 JSON 对象（不规范网关） |
| `sk-legacy` | 第 1 轮用老式 `function_call` 要 `fs_write`，第 2 轮 `content` 给成分片数组 |

环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `LLM_WEB` | http://127.0.0.1:8095 | 被测服务 |
| `MOCK_URL` | http://127.0.0.1:8099/v1 | 假模型 |
| `MOCK_LOG` | /tmp/llm_web_mock_requests.jsonl | 假模型收到的请求（测试会断言这里的内容） |
| `FSTEST_DIR` | /data/hello/tmp/llm_web_tests | 测试用目录，必须在服务的 `--fs-root` 之内 |
| `LLM_WEB_BIN` | /tmp/llm_web_test_bin | `fs_scope_test.py` 要起的那个二进制 |
| `NODE` | node | 跑前端测试用的 node |

> 这台板子编译一次 `llm_web.cpp` 要几十秒，`run_all.sh` 里做了判断：源码没变就直接复用 `/tmp/llm_web_test_bin`。
