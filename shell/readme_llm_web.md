# llm_shell · Web Console（llm_web）

浏览器 → HTTP → llm_web → unix socket → llm_shell daemon。
v1.2.0 起还多了一件事：**把 `/data/hello/os/file` 的 fs_demo 能力桥接进来**，
所以网页上（Files 面板、Chat 里的模型）都能直接新建/修改/删除文件（v1.3.4 起 Files 面板里的删除入口去掉了，见 4）。

v1.2.1 把「被模型捕获」这一段做稳了：不同网关返回的工具调用长得不一样
（arguments 是对象不是字符串、老式 `function_call`、content 是分片数组），
现在都能认出来并真正落盘，见 3.1。

v1.3.0 把文件沙箱的默认范围从 `/data/hello` **放开成 `/`**：默认就能越界（整机都能碰），
Files 面板右上角用一个勾选框显示当前范围。想收窄还是 `--fs-root`，想彻底关掉检查是 `--fs-allow-outside`，见 7。

v1.3.4 把 Files 面板收成"只给浏览"：每行右侧那个「删除」按钮去掉了，前端也不再发删除请求，
面板只能列目录 / 打开文件 / 新建 / 编辑；**浏览范围由「工作区」+ 那个勾选框一起管** ——
勾上时越界连列目录、打开文件都会被拒（顺手修了个真 bug：以前只有"列目录"带「工作区」，
打开 / 保存 / 新建 没带，服务端 `--fs-root` 比「工作区」宽的时候能溜到外面去，见 4、11）。

v1.3.5 把沙箱范围修成**严格「按请求」**：每个请求都从 `--fs-root` / `--fs-allow-outside`
的启动值重新算，再被请求里的 `root` / `allow_outside` 覆盖一次。以前只有默认的
fork-per-connection 才干净 —— `--no-fork` 是同一个进程连着处理所有连接，上一次请求带的
放开状态会残留到下一笔（带了 `allow_outside:true` 之后，后面没带的请求会白捡一个
"越界检查关着"），本该拒的路径就没拒，见 7、11。

v1.3.6 把默认系统提示词统一成一句：`you are a concise(SE5), helpful assistant. 用中文回答`。
全项目就这一处（页面脚本里的 `SYSTEM_PROMPT` 常量），服务端还会在它后面追加工作区/工具说明，
两段拼成最终发给模型的那一条 system 消息，见 3.4。

```
llm_web  ──/api/exec──►  llm_shell daemon（pty 里跑命令，见 ReadMe_llm_shell.md）
   │
   ├──/api/fs──────►  文件系统模块（fs_demo 的 list/read/write/gen/scan + mkdir/append/remove/move/copy/stat）
   │                      └─ 限制在 --fs-root 沙箱内（v1.3.0 起默认 /，即整机）
   └──/api/chat────►  OpenAI 兼容 /chat/completions
                          └─ 模型要工具 → 上面两个模块真正执行 → 结果回灌 → 继续对话
```

---

## 1. 三个面板

| 面板 | 干什么 |
|---|---|
| **Terminal** | 敲命令，走 llm_shell daemon（pty、超时、4MB 截断） |
| **Chat** | 聊天；勾上「文件操作」后模型能真的落盘，勾上「执行命令」后还能跑 shell |
| **Files** | 浏览目录、点开编辑、新建文件/目录；**v1.3.4 起没有删除**（删不了任何东西）；GBK 文件自动识别；右上角那个勾选框能手动收紧/放开沙箱范围（不用重启） |

Chat 顶部的配置：模型 / Key / Base URL / 温度 / **文件操作** / **执行命令** / **工作区** / **工具轮数**，
全部存在浏览器 localStorage（Key 也只留在本地浏览器）。

---

## 2. 编译 & 启动

```bash
cd /data/hello/os/shell
make                       # 编 llm_shell 和 llm_web
```

```bash
# 1) 起命令执行守护进程
nohup ./llm_shell daemon --timeout 30 >/tmp/llm_shell.daemon.log 2>&1 &

# 2) 起 web（默认只监听 127.0.0.1:8080）
nohup ./llm_web --port 8093 --sock /tmp/llm_shell.sock >/tmp/llm_web.log 2>&1 &

# 3) 自检
curl -sS http://127.0.0.1:8093/api/health ; echo
```

本机浏览器打开 <http://127.0.0.1:8093/>；如果 web 跑在板子/容器里，从 PC 上走 SSH 隧道：

```bash
ssh -L 8093:127.0.0.1:8093 linaro@192.168.150.1
# 然后开
 http://127.0.0.1:8093/
```

### 命令行参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port N` | 8080 | 监听端口 |
| `--bind IP` | 127.0.0.1 | 监听地址。**别随便改成 0.0.0.0** |
| `--sock PATH` | /tmp/llm_shell.sock | llm_shell daemon 的 socket |
| `--fs-root PATH` | `/` | 文件工具/文件面板的工作区（沙箱根）。v1.3.0 起默认 `/`，也就是能操作整机的文件 |
| `--fs-allow-outside` | 关 | 关掉越界检查：`--fs-root` 之外的路径也放行。**很危险**（等于没有任何限制） |
| `--max-tool-rounds N` | 8 | 一次对话最多几轮工具调用（1~20） |
| `--chat-timeout N` | 180 | Chat 一轮对话的**时间预算**秒数（5~300）。这是网页上「超时(s)」输入框默认值与上限的出处；网页里改的是「这一次对话」的值 |
| `--no-fork` | 关 | 串行处理连接。默认每个连接 fork 一个子进程，所以聊天卡住时终端面板照样能用。v1.3.5 起串行模式下沙箱范围也严格按请求重算（不留上一笔的 `root` / `allow_outside`） |

> 监听所有网卡（仅限可信内网，临时用）：
> `./llm_web --port 8093 --bind 0.0.0.0 --sock /tmp/llm_shell.sock`
> 这等于把「能建文件、能跑命令」的接口暴露给整个网段，用完立刻改回 `127.0.0.1`。

---

## 3. Chat 里的工具调用（这次的桥接核心）

勾上「文件操作」，模型就会拿到这些工具（名字 = `/api/fs` 的 action，一一对应）：

| 工具 | 作用 | 对应 fs_demo |
|---|---|---|
| `fs_list` | 列目录（名字/类型/大小/mtime） | `fs_demo list` |
| `fs_read` | 读文本（自动 UTF-8/GBK，二进制给 base64） | `fs_demo read` |
| `fs_write` | 新建/覆盖写（自动建父目录） | `fs_demo write` |
| `fs_append` | 追加 | — |
| `fs_mkdir` | 建目录（多级） | — |
| `fs_remove` | 删文件/目录（目录要 `recursive:true`）。**只有模型这条路**，网页 Files 面板没有删除入口（v1.3.4） | — |
| `fs_move` / `fs_copy` | 移动重命名 / 复制 | — |
| `fs_scan` | 递归扫描 + 扩展名/深度过滤（动手前先看结构） | `fs_demo scan` |
| `fs_gen` | 模板渲染生成文件（`{{VAR}}`、`{{__FILENAME__}}` 等） | `fs_demo gen` |
| `run_shell` | 执行命令（走 daemon，带超时）——需另勾「执行命令」 | — |

服务端做的事（`chatWithTools()`）：

1. 把前端发来的 `messages` 加上一段 system 提示（告诉模型工作区在哪、必须真调工具、别编结果；
   默认那句"你是简洁助手、用中文回答"由前端带、见 3.4）；
2. 带上 `tools` + `tool_choice:"auto"` 调 `/chat/completions`；
3. 模型回 `tool_calls` → **在本地真正执行**（沙箱文件操作 / daemon 命令）；
4. 把每个结果按 `{"role":"tool","tool_call_id":...}` 回灌，再问一次；
5. 直到模型给出文字答复，或到达轮数上限 / 时间预算。

细节：

- **上限**：`max_rounds`（默认 8）按对话算，超了会回一句「已到上限」，可以再说一句「继续」；
- **死循环保护**：同一个工具 + 同一参数重复第 3 次会被拒（`判定为死循环`）；
- **时间预算**：整轮请求不超过 `timeout`（网页「超时(s)」输入框，默认 180s，允许 5~300s；
  `--chat-timeout` 定默认值，服务端还会自己夹一遍），**所有工具轮次加起来**共用这一份预算；
  单轮 LLM 调用可以用满剩余预算（不再有隐藏的固定上限），回包里的 `timeout` 是实际生效值；
- **单次结果截断**：工具返回值最多 32KB 喂回模型，`fs_read` 默认最多读 256KB；
- **每个用户回合独立**：历史里的工具调用不会回放给模型（只回放文字），所以上下文不会被工具噪音撑爆；
- 模型不支持 function calling 的话，把「文件操作」关掉就是原来的纯聊天，不会被 400。

### 3.1 工具调用形态兼容（v1.2.1）

桥接能不能用，取决于服务端认不认得出模型/网关返回的工具调用。实践中这几类都真实出现过，
v1.2.0 会静默丢掉其中两种（表现为「模型说要建文件，但磁盘上什么都没有」）：

| 网关返回的形态 | v1.2.0 的行为 | v1.2.1 的行为 |
|---|---|---|
| `arguments` 是 JSON **对象** `{...}`，不是字符串 | 当成「没参数」执行 → `ok:false, 缺少 content`；回灌给模型的 `arguments:""` 还常被判 400 | 归一化成字符串，正常执行 |
| 老式 `function_call: {name, arguments}`（不是 `tool_calls`） | 整个调用被丢掉，回「（模型没有返回内容）」 | 合成成一次 tool call，正常执行 |
| 最终答复 `content` 是分片数组 `[{"type":"text","text":...}]` | 读成空串 → 「（模型没有返回内容）」 | 按顺序拼成文本 |
| 参数被多包了一层字符串 | 报参数不合法 | 再解一层后执行 |

**为什么要真调工具**：
模型经常会在没落盘的情况下说"已经帮你创建好了"。现在服务端在 system 提示里明确要求
"必须调用 fs_write 真正落盘，不要只回复已创建"，并且网页会把每次工具调用显示成可折叠的轨迹（✓/✗ + 参数 + 返回 JSON），
谁在说真话一眼就能看出来。

### 3.2 「[错误] HTTP 100」是什么（v1.3.1 修）

看到聊天里回一句 `[错误] HTTP 100`，**不是上游真的返回了 100**，而是这一轮 LLM 调用超时/连接被掐断了。

原因：请求体超过 1KB 时 curl 会自动加 `Expect: 100-continue`，上游先回一个 `HTTP/1.1 100 Continue`（中间响应，属正常流程），
正文随后才真正开始处理。如果这一轮在这时被 `--max-time` 掐掉（或上游把连接断了），curl 以非 0 退出，
而 `%{http_code}` 停在了最后收到的那个中间响应上 —— 也就是 **100**。旧代码只把 `http==0` 当失败，
于是超时被当成状态码原样甩给前端，真正的「超时」两个字就被盖掉了。

复现（本机实测，板子到上游上行约 70KB/s）：

```bash
# 3.8MB 请求体 + 30s 上限，旧版（.cpp.v1.3.0.pre_http100fix.bak 编出来的）:
#   {"ok":false,"http_code":100,"error":"HTTP 100"}
# 新版:
#   {"ok":false,"http_code":0,
#    "error":"本轮 LLM 调用超时或中断（上限 30s）：curl: (28) Operation timed out after 30001 milliseconds..."}
```

这次的改动（`callLLM` / `callLLMApi` 两条路都改）：

- curl 参数加 `-H "Expect:"`，直接不做 100-continue 探测，从根上避免这个中间响应；
- `ran==false`（curl 没正常退出）或 `http_code` 是 100/0 时，一律按「超时或中断」报，并把 curl 的 stderr 摘要附上；
- 单轮 LLM 调用上限 90s → **150s**（上行慢 + 上下文大时，一轮本来就要 40~90s，90s 卡在临界点上）；
  v1.3.2 起进一步改成「单轮 = 剩余预算」，这个 150s 的硬编码已经删掉；
- 真正拿到 HTTP 状态码（400/401/429/5xx…）时，照旧原样上报，不受影响。

要根治「动不动就超时」，还得从**请求体**下手：上行只有 ~70KB/s，1MB 的上下文光上传就要 15s。
少开几轮工具、别让模型一次读大文件（`fs_read` 的 `max_bytes` 默认 256KB，读视频/模型文件没有意义）、
把长任务拆成几句「继续」，都比调大超时管用。

### 3.3「超时(s)」输入框（v1.3.2）

Chat 配置区 模型/Key/Base URL/温度/文件操作/执行命令/工作区/工具轮数 后面多了一个 **超时(s)**：

- 含义是**一整轮对话的时间预算**（含所有工具轮次），不是单次 curl 的超时；
- 范围 **5~300s**，默认 **180s**。填超范围会在输入框里就地夹紧并在状态栏说一句；
  服务端 `/api/chat` 也会自己夹一遍，不靠前端自觉；
- 默认值来自服务端：页面加载时 `/api/health` 报 `chat_timeout` / `chat_timeout_max`，
  本机没存过配置就跟着它走（想改机器级的默认值：`--chat-timeout 240` 重启服务）；
- 值存在浏览器 `localStorage`（`llm_web_chat_conf_v2` 的 `timeout` 字段），和别的配置一起保存/回车保存；
- 发送时状态栏会显示「预算 Ns」，跑完显示的是**服务端实际用的**预算（`/api/chat` 回包带 `timeout`）；
- 预算真用完时：已经拿到的答复会原样返回并附一句「已到时间上限，可以再说一句继续」，
  而不是把进度丢掉换成一句超时错误。

> 调大它只是给「本来就慢」的请求更多时间，不能把慢请求变快：上行只有 ~70KB/s，
> 1MB 上下文光上传就要 15s。长任务（跑视频推理、编译）正确姿势是丢后台 + 多问几句「继续」。

---

### 3.4 默认系统提示词（v1.3.6）

默认那句**全项目只有一处**：`llm_web.cpp` 里页面脚本的那个前端常量，跟着每次请求发出去 ——

```js
const SYSTEM_PROMPT = 'you are a concise(SE5), helpful assistant. 用中文回答';
```

改完要 `make` + 重启（HTML 是编进二进制里的）；页面上没有能改提示词的输入框。

服务端另外还会拼一段"你在这个工作区里、工具怎么用"的说明（`injectToolPrompt()`）：请求里已经有
system 就**追加在后面**，没有就新建一条。这段是动态的 —— 里面写着运行时的「工作区根目录：」；
勾了「文件操作」才有 `fs_write / fs_mkdir / fs_read / fs_list / fs_scan` 那几句；
勾了「执行命令」才会多出 `run_shell` 那句。

所以模型收到的 system 消息 = 上面那句 + 这段工具说明，**两段拼成一条**（`tests/chat_ui_test.js`
按这个断言：第一段逐字比对，且整个请求只有一条 system）。

## 4. Files 面板

- 顶部输入路径回车打开（相对路径按工作区根目录算）；「上级」「刷新」「新建文件」「新建目录」；
- 「工作区」（Chat 配置区那个输入框）对面板里**每一个**操作都生效（v1.3.4 起）：列目录、打开文件、
  保存、新建都按「工作区」+ 勾选框算范围。以前只有"列目录"带着它，服务端 `--fs-root` 比「工作区」
  宽（v1.3.0 起默认就是 `/`）时，**打开文件能溜到工作区外面去**（列不出来但点得到，编辑器里就出现了
  外面的内容）—— 现在统一都带 `root`，读也吃同一份范围；新建时名字里写 `..` 也绕不出去
  （服务端按规范化之后的真实路径判越界，越界就直接拒、磁盘上不落盘）；
- 点目录名进下一层，点文件名打开编辑器；
- 编辑器：`Ctrl/Cmd+S` 保存；保存时可选编码与换行：
  - **编码**：打开时自动识别，默认按原编码存回（UTF-8 / GBK 不会互相改坏）；
  - **换行**：`保留` / `CRLF` / `LF` —— 这个工程的老文件是 CRLF，新建同类文件记得选 CRLF；
- **面板里没有删除功能**（v1.3.4 起）：每行右侧原来那个「删除」按钮已经去掉，前端也不再发删除请求，
  面板只能列目录 / 打开文件 / 新建 / 编辑；
- 「只许浏览」也包括**读**：越界时不止写会被拒，列目录、打开文件一样被拒（提示里写明范围），
  所以勾选框管的是"能看什么"+"能改什么"这一整块；
- 真要删文件，面板不是唯一的路：Terminal 面板 `rm`，或者 `curl` 打 `/api/fs` 的 `remove`，
  Chat 里勾了「文件操作」的模型手上也还有 `fs_remove`（见 3、5）—— 这次只是把网页这一块的入口收掉了；
- 右上角那个「只允许操作 X 内的路径」勾选框是**真开关**（v1.3.3 起，以前是只读展示）：
  **勾着** = 只允许操作当前工作区内的路径，越界直接拒绝；**取消勾选** = 放开越界
  （等价启动时加 `--fs-allow-outside`），整块变黄、文案变成「已放开越界限制（勾上可重新收紧）」；
  勾/取消**下一次操作立刻生效，不用重启服务**；
- v1.3.5 起服务端保证它是**严格的这一次请求**：每个请求都从启动参数重算一遍，所以就算服务端
  用了 `--no-fork`（同一个进程连着处理），你这次的勾选也不会漏给下一笔请求；
- 它是浏览器本地配置：跟「工作区」「超时(s)」一起存 localStorage（`llm_web_chat_conf_v2` 的
  `outside` 字段），并且跟着**每个请求**带 `allow_outside`，所以只影响你自己这一次操作，
  终端面板和别人的请求不受影响。本机没存过配置时跟着服务端走 —— 启动参数 `--fs-allow-outside`
  现在只决定这个默认值；
- 注意「工作区」默认是 `/`：根是 `/` 时没有"外面"，勾不勾都一样。想看出区别先把「工作区」
  收窄，比如填 `/data/hello`，再取消勾选就能浏览整机；
- 所有操作都走 `/api/fs`，越界（工作区之外）会被直接拒绝，提示里写明范围：
  `越界：只允许操作 / 内的路径（要放开就加 --fs-allow-outside）`。

---

## 5. HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 页面 |
| GET | `/api/health` | daemon 是否活着；顺带报 `fs_root` / `fs_allow_outside` / `chat_timeout` / `chat_timeout_max` |
| POST | `/api/exec` | `{"command":"uname -a","timeout":30}` |
| GET | `/api/fs?action=list&path=/data/hello` | 只读快捷方式，方便 curl |
| POST | `/api/fs` | 文件操作，见下表 |
| GET | `/api/tools` | 看看当前能开出哪些工具（含 `chat_timeout` / `chat_timeout_max`） |
| POST | `/api/chat` | 聊天 / 工具调用 |

### `/api/fs` 的 action

```bash
# 列目录
curl -sS -X POST http://127.0.0.1:8093/api/fs -H 'Content-Type: application/json' \
     -d '{"action":"list","path":"/data/hello/os/shell"}'

# 新建文件（等价 fs_demo write；自动建父目录）
curl -sS -X POST http://127.0.0.1:8093/api/fs -H 'Content-Type: application/json' \
     -d '{"action":"write","path":"/data/hello/tmp/hello.txt","content":"你好\n","eol":"crlf"}'

# 读（默认自动识别编码）
curl -sS -X POST http://127.0.0.1:8093/api/fs -H 'Content-Type: application/json' \
     -d '{"action":"read","path":"/data/hello/tmp/hello.txt"}'

# 递归扫描
curl -sS -X POST http://127.0.0.1:8093/api/fs -H 'Content-Type: application/json' \
     -d '{"action":"scan","path":"/data/hello/os/shell","ext":".cpp,.h","depth":2}'

# 模板生成（等价 fs_demo gen）
curl -sS -X POST http://127.0.0.1:8093/api/fs -H 'Content-Type: application/json' \
     -d '{"action":"gen","template":"/data/hello/tmp/tpl.cpp","output":"/data/hello/tmp/new.cpp","vars":{"NAME":"llm"}}'
```

| action | 关键参数 |
|---|---|
| `list` | `path` |
| `read` | `path`，可选 `charset`(auto/utf-8/gbk)、`max_bytes` |
| `write` | `path`、`content`，可选 `charset`(utf-8/gbk)、`eol`(keep/lf/crlf)、`mkdirs` |
| `append` | 同 `write` |
| `mkdir` | `path` |
| `remove` | `path`，可选 `recursive` |
| `move` / `copy` | `path`(源)、`to`(目标)，可选 `overwrite` |
| `stat` | `path` |
| `scan` | `path`，可选 `ext`、`depth`、`max_entries` |
| `gen` | `template`、`output`，可选 `vars`、`charset`、`eol` |

任何请求都可以额外带 `"root":"/data/hello/os"` 临时改这次请求的工作区（沙箱跟着变，越界判断也一起变）；
也可以带 `"allow_outside":true/false` 临时开关这次请求的越界检查 —— 网页 Files 面板那个勾选框就是这么做
（GET 形式写 `&allow_outside=1`，另外也认 `0/true/false/on/off/yes/no`，写别的会直接报 `allow_outside 非法`）。
回包里的 `fs_root` / `fs_allow_outside` 报的是**这次请求实际生效的**值。

> 网页 Files 面板从 v1.3.4 起没有删除入口，但 `/api/fs` 的 `remove` action 本身还在
> （`curl` 和 Chat 里模型的 `fs_remove` 都用它）—— 去掉的只是面板上那个按钮。

### `/api/chat`

```bash
# 纯聊天（老格式返回 {"http_code":..,"body":"..."}）
curl -sS -X POST http://127.0.0.1:8093/api/chat -H 'Content-Type: application/json' -d '{
  "model":"gpt-4o-mini","api_key":"sk-xxx","base_url":"https://api.openai.com/v1",
  "messages":"[{\"role\":\"user\",\"content\":\"你好\"}]"
}'

# 带工具：模型可以直接在你工作区里建文件
curl -sS -X POST http://127.0.0.1:8093/api/chat -H 'Content-Type: application/json' -d '{
  "model":"gpt-4o-mini","api_key":"sk-xxx","base_url":"https://api.openai.com/v1",
  "messages":"[{\"role\":\"user\",\"content\":\"在 /data/hello/os/shell 下建个 notes.md，写三条待办\"}]",
  "tools":{"fs":true,"shell":false}, "root":"/data/hello", "max_rounds":8, "timeout":180
}'
```

`timeout` 是**一整轮对话的时间预算**（秒，含所有工具轮次）：不给就用服务端的 `--chat-timeout`，
超出 5~300 一律夹到边界。回包里的 `timeout` 是实际生效值，`timeout_max` 是上限。
预算用完时 `hit_limit:true` 且 `ok:true`（把手上已有的答复给你）；只有真把某一轮调崩了才会 `ok:false`。

带工具时返回：

```json
{"ok":true,"http_code":200,
 "content":"已经建好 notes.md，写了三条待办。",
 "tool_log":[{"round":1,"tool":"fs_write","args":"{...}","ok":true,"result":"{\"ok\":true,...}","ms":0}],
 "rounds":1,"hit_limit":false,"timeout":180,"timeout_max":300,
 "fs_root":"/","fs_allow_outside":false,"shell":false,
 "body":"<最后一轮 LLM 原始响应>"}
```

---

## 6. 编码与换行（这个工程的坑）

- 页面本身是 UTF-8；命令输出/文件名如果是 GBK，服务端会按"非 ASCII 连续段"自动判定并转成 UTF-8（`toUtf8()`），
  所以 `ls` 出来是 GBK 名字也不会把 JSON 弄坏；
- `/api/fs` 的 `read` 默认自动识别编码，`write` 可用 `charset:"gbk"` 写回 GBK（本仓库部分老源文件是 GBK，
  Files 面板打开后默认按原编码存回，不会把你的 GBK 文件悄悄转成 UTF-8）；
- 找文件时如果 UTF-8 路径不存在，会自动再用 GBK 字节试一次，所以老工程里的 GBK 文件名也能点开；
- 换行：`eol` 支持 `keep/lf/crlf`。本仓库的老文件是 CRLF，用 `fs_gen`/`fs_write` 新建 `.cpp/.md` 时建议 `"eol":"crlf"`。

---

## 7. 安全（别忽略）

- **默认已经不再限制在 /data/hello 了**：v1.3.0 起沙箱根默认是 `/`，也就是说模型/网页能读写整机的**任何**文件
  （以 `llm_web` 那个用户的权限为准）。想收回项目目录：`--fs-root /data/hello`；
- 只要 `--fs-root` 是一个具体目录，文件工具就**只能动它之内**的路径：`..` 穿越、绝对路径越界、软链接指向外面都会被拒
  （`weakly_canonical` 之后再判）；根是 `/` 的时候没有"外面"，这个检查自然全放行；
- 越界检查（v1.3.3 起）是**按请求**的：网页上那个勾选框、或者请求里的 `allow_outside`，都能在这一
  次请求里把限制收紧或放开，`--fs-allow-outside` 只决定默认值；要钉死范围就别靠它，
  请在服务外面限制（只监听 127.0.0.1、换账号跑、防火墙）；
- 任何请求都能带一个 `"root":"..."` 临时改这次请求的沙箱根（Files 面板的"工作区"输入框就是这么用的），
  所以**别把这个服务暴露给不信任的人**，它不是一个可靠的安全边界；
- 删目录必须显式 `recursive:true`，沙箱根目录本身拒绝删除；v1.3.4 只是去掉了网页 Files 面板里的删除入口，
  API 与模型那一侧的删除能力照旧（Terminal 面板本来就能 `rm`）；
- 命令执行**默认关闭**，只有 Chat 里勾了「执行命令」才会给模型 `run_shell`（curl 直接调 `/api/exec` 不受这个开关限制，它本来就是终端面板用的）；
- 这个 web 没有任何登录鉴权：**只监听 127.0.0.1 + SSH 隧道**是唯一推荐的用法；
- `llm_shell` 以当前用户身份跑命令，不是 root，也不需要 root（要提权请自己单独设计）。

---

## 8. 和 systemd 一起开机自启

```bash
sudo tee /etc/systemd/system/llm-shell.service > /dev/null <<'EOF'
[Unit]
Description=LLM Shell daemon
After=network.target

[Service]
Type=simple
User=linaro
Group=linaro
WorkingDirectory=/data/hello/os/shell
ExecStart=/data/hello/os/shell/llm_shell daemon --timeout 30
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

sudo tee /etc/systemd/system/llm-web.service > /dev/null <<'EOF'
[Unit]
Description=llm_shell Web Console
After=llm-shell.service
Wants=llm-shell.service

[Service]
Type=simple
User=linaro
Group=linaro
WorkingDirectory=/data/hello/os/shell
ExecStart=/data/hello/os/shell/llm_web --port 8093 --sock /tmp/llm_shell.sock --fs-root / --chat-timeout 180
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now llm-shell llm-web
```

改了源码之后：

```bash
cd /data/hello/os/shell && make
sudo systemctl restart llm-shell llm-web
systemctl status llm-shell llm-web --no-pager
journalctl -u llm-web -n 30 --no-pager
```

**别混用**：已经启用 systemd 服务就不要再 `nohup ./llm_web ...`，
两个进程会抢同一个端口（`bind: Address already in use`）和同一个 socket 路径。

---

## 9. 测试

```bash
cd /data/hello/os/shell
make test              # 等价 sh tests/run_all.sh
```

`tests/run_all.sh` 会：编一个测试用二进制 → 在 8095 起服务（工作区 /data/hello）→ 起一个假模型端点（8099）→
跑六套测试 → 自己收尾（不碰你正在用的 8093）。`fs_scope_test.py` 会另外在 8101/8102/8103/8104
各起一个不同参数的实例（默认 / / `--fs-root` / `--fs-allow-outside` / 给 D 段那个收窄沙箱），
跑完自己收掉；最后一段要假模型 8099 在跑，没跑就 SKIP。

| 测试 | 覆盖 |
|---|---|
| `tests/fs_smoke.py` | `/api/fs` 全部 action、GBK 读写、CRLF、base64、gen 模板、沙箱越界、GET 形式、错误处理（34 项） |
| `tests/ui_test.js` | 前端 Files 面板：进目录、打开、改内容保存、新建文件/目录、越界提示；**面板里没有删除入口**（行里没按钮、脚本里不再发 `remove`、刷新后文件还在）；浏览（列目录 / 打开文件）也被沙箱管着；勾选框手动勾/取消后真的放开与收紧；「工作区」+ 勾选框管着所有操作（收窄后打开工作区外的文件被拒，服务端 `--fs-root` 更宽也不行；「新建文件」写工作区外也被拒且磁盘上不落盘，工作区里面照常能建）（52 项，用 Node 打桩 DOM 真连服务） |
| `tests/chat_ui_test.js` | 前端 Chat：点发送 → 带工具请求 → 文件真的落盘 → 轨迹渲染；「超时(s)」输入框的默认值/夹紧/存盘/回显；**默认系统提示词**（v1.3.6：发给模型的第一段逐字 = `you are a concise(SE5), helpful assistant. 用中文回答`，且整条请求只有一条 system）（24 项） |
| `tests/tool_shapes_test.py` | 工具调用形态兼容：arguments 给成对象、老式 `function_call`、数组 content、参数二次解包（14 项） |
| `tests/fs_scope_test.py` | 沙箱范围：默认根 `/` 能越界读写、`--fs-root` 收窄后越界被拒且文案正确、`--fs-allow-outside` 关掉检查；请求级 `allow_outside`（v1.3.3）能临时放开/收紧、非法值报错，连 `/api/chat` 里模型的 `fs_*` 工具一起验；顺带用 `tests/scope_ui_test.js` 手动勾/取消勾选框；再验 `--no-fork` 下范围严格按请求（上一笔的 `root` / `allow_outside` 不许残留，v1.3.5）（75 项） |
| `tests/chat_timeout_test.py` | Chat「超时(s)」语义（v1.3.2）：health/tools 报默认值与上限、越界夹紧、回包带生效值；再对着一个"收到请求就装死"的上游跑 `timeout=5`，验证 5 秒出头就报「超时或中断」而不是干等、更不会变成老 bug 那句 `HTTP 100`（15 项） |

`tests/mock_llm.py` 是个假的 OpenAI 兼容端点，用 `api_key` 切换行为（`sk-test` 建文件、`sk-shell` 跑命令、
`sk-loop` 一直要工具、`sk-dup` 重复同一调用、`sk-objargs` arguments 给成对象、
`sk-legacy` 老式 function_call + 数组 content），想手动试也可以：

```bash
python3 tests/mock_llm.py &
# 网页 Chat 里填: 模型 mock-1 / Key sk-test / Base URL http://127.0.0.1:8099/v1
```

---

## 10. 排障

| 现象 | 原因 / 处理 |
|---|---|
| 浏览器连不上 | 默认只监听 127.0.0.1 → 用 SSH 隧道，或临时 `--bind 0.0.0.0` |
| `bind: Address already in use` | 又手动起了一份。`systemctl restart llm-web`，或者先 `ss -lntp \| grep 8093` 看看是谁 |
| 页面能开，但执行命令报 daemon down | `llm_shell daemon` 没跑；`systemctl restart llm-shell` |
| Files 面板说"越界" | 路径不在 `--fs-root` 里。默认根是 `/`，正常碰不到这句；一旦被拒，看提示里写的范围。要么换路径，要么启动时改 `--fs-root`（或在请求里带 `root`；彻底放开是 `--fs-allow-outside`） |
| 右上角勾选框是空的 / 发黄 | 现在是"放开越界"状态：要么启动时带了 `--fs-allow-outside`，要么你自己把勾选框点掉了。**勾上就立刻收紧，不用重启**（状态存在浏览器 localStorage，想回到跟启动参数一致就清掉 `llm_web_chat_conf_v2`） |
| Files 面板里找不到「删除」 | v1.3.4 起面板故意没有这个入口（只能浏览 / 新建 / 编辑）。要删：Terminal 面板 `rm`，或 `curl -X POST .../api/fs -d '{"action":"remove","path":"..."}'`，或让 Chat 里勾了「文件操作」的模型删 |
| 中文乱码 | 看是页面乱码还是命令输出乱码：命令输出走 `toUtf8()` 自动转；文件内容用 Files 面板的"编码"下拉选对 |
| Chat 一直"调用失败 HTTP 400" | 有些网关不支持 `tools`。把「文件操作」关掉回到纯聊天；或者换支持 function calling 的模型 |
| Chat 说建好了但没文件 | 看轨迹块（✓/✗）。`ok:false` 就是没建成功，`error` 里写了原因（多半是越界或路径写错） |
| Chat 报「本轮 LLM 调用超时或中断（上限 Ns）」 | 这份预算用完了。先看是不是上下文太大（上行 ~70KB/s）：少开几轮工具、别 `fs_read` 大文件（视频/模型）；真要长跑就把长任务 `nohup` 丢后台，再发「继续」看日志。确实需要更长时间就调大「超时(s)」（5~300） |
| 「超时(s)」填了 600 却按 300 跑 | 正常：上限就是 300s，输入框会就地夹到 300 并在状态栏说一句 |

---

## 11. 版本

| 版本 | 变化 |
|---|---|
| v1.1.0 | Terminal + Chat 两个面板；GBK 输出转 UTF-8；`/api/exec`、`/api/chat` |
| **v1.2.0** | 桥接 `file` 模块：新增 `/api/fs` 与 Files 面板；Chat 支持 function calling（fs_* + run_shell）；沙箱与越界拒绝；fork-per-connection；`/api/tools`；`tests/` 回归测试 |
| **v1.2.1** | 工具调用形态兼容：`arguments` 对象 / 老式 `function_call` / 数组 `content` / 参数二次解包；新增 `tests/tool_shapes_test.py`（回归 65 → 79 项） |
| **v1.3.0** | 沙箱默认范围放开：`--fs-root` 默认 `/`（可以越界，整机都能操作）；Files 面板加只读勾选框显示当前范围；`/api/health`、`/api/fs`、`/api/tools`、`/api/chat` 都返回 `fs_root` / `fs_allow_outside`；修掉根为 `/` 时 `withinRoot` 把 `/x` 误判成越界的老 bug；新增 `tests/fs_scope_test.py` + `tests/scope_ui_test.js` |
| **v1.3.1** | 修「`[错误] HTTP 100`」：`curl` 加 `-H "Expect:"`，不再拿 100 Continue 中间响应冒充最终状态码；`ran==false` / `http_code` 100、0 一律按「超时或中断（上限 Ns）」上报并附 curl stderr 摘要；单轮调用上限 90s → 150s（`/data/hello/os/shell/readme_llm_web.md` §3.2） |
| **v1.3.2** | Chat 配置区新增**「超时(s)」输入框**（5~300s，默认 180）：整轮对话的时间预算，存 localStorage、回车即存、越界就地夹紧并提示；默认值由服务端 `/api/health` 的 `chat_timeout` 给出，新增 `--chat-timeout N`；`/api/chat` 回包带生效的 `timeout` / `timeout_max`；删掉工具路径里"单轮最多 150s"和"预算至少 30s"两个隐藏地板（现在单轮 = 剩余预算，报的就是用的）；新增 `tests/chat_timeout_test.py`，`chat_ui_test.js` 覆盖输入框（回归 126 → 149 项） |
| **v1.3.3** | Files 面板那个沙箱勾选框从"只读展示"变成**真开关**：勾上 = 只允许操作工作区内的路径，取消勾选 = 放开越界（等价 `--fs-allow-outside`），下一次操作立刻生效、**不用重启**；状态存 localStorage 的 `outside`、跟着每个请求带 `allow_outside`（`/api/fs` 与 `/api/chat` 都认，Chat 里模型的 `fs_*` 工具也一样受管），`--fs-allow-outside` 只定默认值；`tests/scope_ui_test.js` 重写为“手动勾/取消真的放开与收紧”，`fs_scope_test.py` 加请求级 allow_outside 与 chat 工具用例（回归 149 → 185 项） |
| **v1.3.4** | Files 面板去掉**「删除」功能**：列表不再有删除按钮、前端不再发 `remove`，那 5 列变 4 列（名称/类型/大小/修改时间）；面板只剩浏览入口（列目录/打开文件/新建/编辑），浏览范围照旧由勾选框管（勾上时越界连列目录、打开文件都拒）。顺手修了个范围 bug：面板里只有"列目录"带「工作区」（`root`），打开 / 保存 / 新建 没带，所以服务端 `--fs-root` 比「工作区」宽（默认 `/`）时这几个操作能碰到工作区外面的文件 —— 现在每个 `/api/fs` 请求都带 `root`，读也吃同一份范围。后端 `/api/fs` 的 `remove` 与 Chat 里模型的 `fs_remove` 没动（要一起禁掉另说）；`tests/ui_test.js` 跟着改（删除用例 → “没有删除入口 + 越界连打开都被拒 + 收窄「工作区」后打开外面的文件被拒 + 「新建文件」写工作区外也被拒且不落盘”，回归 185 → 206 项） |
| **v1.3.5** | 沙箱范围改成**严格的按请求语义**：每个请求开头都从 `--fs-root` / `--fs-allow-outside` 的启动值重算一次，再让请求里的 `root` / `allow_outside` 覆盖。修的是 `--no-fork` 下的残留 bug：串行模式同一个进程连着处理所有连接，上一次请求带的放开状态会留给下一笔 —— 实测“先带 `allow_outside:true`，再带 `root:/data/hello` 列 `/etc`”原本会放行（等于把「只允许操作 X 内的路径」绕过去），现在照旧拒绝；请求里的 `root` 也不再残留。默认 fork 模式行为不变；`fs_scope_test.py` 新增 E 段（回归 206 → 212 项） |
| **v1.3.6** | 默认系统提示词统一成一句 **`you are a concise(SE5), helpful assistant. 用中文回答`**：改的是页面脚本里的 `SYSTEM_PROMPT` 常量（全项目唯一一处；服务端 `injectToolPrompt()` 追加的"工作区/工具说明"没动，两段拼成最终那条 system 消息）。readme 新增 §3.4 说明"想改提示词改哪一行"，`tests/chat_ui_test.js` 加两条断言：发给模型的第一段逐字比对、整个请求只有一条 system（回归 212 → 214 项） |
