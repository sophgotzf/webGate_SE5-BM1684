## 3. 编译

cd /data/hello/os/shell
g++ -std=c++17 shell.cpp -o shell


---

## 4. 运行

### 4.1 进入交互式 shell

./shell


程序会先打印环境信息，然后把你丢进一个真正的 $SHELL（通常是 /bin/bash），你可以像平常一样敲命令。退出方式：

exit


或者按 Ctrl-D。

### 4.2 执行单条命令

./shell -c "ls -l /data/hello"
./shell -c "whoami"
./shell -c "uname -a"


---

## 5. 关键 API 说明

| API | 作用 |
|---|---|
| fork() | 复制当前进程，返回 0 表示子进程，>0 是子进程 PID，<0 失败 |
| execvp(file, argv) | 用新程序替换当前进程镜像；argv[0] 是程序名 |
| waitpid(pid, &status, 0) | 等待子进程结束，拿回退出码 |
| isatty(fd) | 判断文件描述符是否连着终端 |
| system(cmd) | 调 /bin/sh -c cmd，简单但会阻塞并受信号影响 |
| getuid() / getpwuid() | 获取当前用户 UID 与用户名 |

---

## 6. 常见报错 & 处理

1. execvp 失败: No such file or directory
说明 $SHELL 指向的路径不存在。可以直接指定一个：

SHELL=/bin/bash ./shell


或把 sys 里默认路径改成 /bin/bash。

2. 交互模式下提示符不刷新 / TERM 未设置
通过 ssh 或某些终端运行会丢 TERM，导致无颜色或无提示符：

export TERM=xterm-256color
./shell


3. 通过脚本/管道运行报“stdin 不是终端”
这是预期行为。管道里 stdin 不是 tty，bash 不会以交互模式启动。改成：

./shell -c "命令"


4. 权限报错 Operation not permitted
如果你把程序设了 setuid 之类再运行来“获取权限”，现代 Linux 会做很多限制；本示例是正常启动一个当前用户身份的 shell，不涉及提权。

---

## 7. 加一个 Makefile（可选）

cat > /data/hello/os/shell/Makefile <<'EOF'
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
TARGET   := shell
SRC      := shell.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
EOF


之后：

cd /data/hello/os/shell
make
./shell


---

如果你想要的其实是别的含义的“获取终端权限”，比如：

- 程序被别的进程调用时能拿到调用者的 tty（/dev/tty、ttyname()、tcgetattr()）；
- 在无 tty 环境下用 openpty() 自己造一个伪终端（pty）跑 shell；
- 通过 posix_spawn + 会话管理让 shell 完全脱离当前终端；

告诉我具体场景，我可以把代码改成对应方向的版本。



## ## ## ## #### ## ## ## ##  安全须知（先看这个）## ## ## ## #### ## ## ## ## 

这个程序等于把你的 shell 交给任何能连上该 socket 的进程。请务必：

- socket 只给当前用户（chmod 0600），且不要绑定到 TCP 端口暴露到网络
- 认真看 /tmp/llm_shell.log（默认会记录所有命令）
- 不要用 root 运行；需要提权场景请单独设计
- 真要跑不可信命令，请再套一层 bwrap / firejail / docker 沙箱（文末给示例）

## ## ## ## #### ## ## ## ##  安全须知（先看这个）## ## ## ## #### ## ## ## ## 
cd /data/hello/os/shell
g++ -std=c++17 llm_shell.cpp -o llm_shell -lutil


## 4. 运行

### 4.1 启动守护进程（放在一个终端里）

./llm_shell daemon --timeout 30
# [llm_shell] listening on /tmp/llm_shell.sock (default timeout=30s)
# [llm_shell] log=/tmp/llm_shell.log


### 4.2 另开一个终端，执行命令

./llm_shell exec "uname -a"
./llm_shell exec "pwd && ls -la /data/hello"
./llm_shell exec "echo hi > /data/hello/os/shell/123.txt"
./llm_shell exec --timeout 5 "sleep 100"   # 会在 5s 后被杀



cd /data/hello/os/shell
pkill -f 'llm_shell daemon' 2>/dev/null
sudo rm -f /tmp/llm_shell.sock 2>/dev/null
nohup ./llm_shell daemon --timeout 30 >/tmp/llm_shell.daemon.log 2>&1 &
sleep 0.5
./llm_shell exec "uname -a"
echo "exit=0"
pkill -f 'llm_shell daemon'


### 4.3 查看信息 / 停止

./llm_shell info
# 停止：在守护进程所在终端按 Ctrl-C（会自动删除 socket）


---

## 5. 给大模型调用（示例）

### 5.1 直接命令行工具

一个最小的 Python 工具（例如给 Claude/OpenAI function calling，或 MCP server）:

# tools/run_terminal.py
import subprocess

def run_terminal(command: str, timeout_sec: int = 30) -> dict:
    """
    在本地终端执行命令。
    :param command: 要执行的 shell 命令
    :param timeout_sec: 超时秒数
    :return: {"exit_code": int, "output": str}
    """
    p = subprocess.run(
        ["/data/hello/os/shell/llm_shell",
         "exec", "--timeout", str(timeout_sec), command],
        capture_output=True, text=True
    )
    return {
        "exit_code": p.returncode,
        "output": p.stdout + ("\n[stderr]\n" + p.stderr if p.stderr else "")
    }


### 5.2 OpenAI / Anthropic 风格的函数定义

{
  "name": "run_terminal",
  "description": "在本机终端上执行 shell 命令，返回 stdout/stderr 与退出码。",
  "parameters": {
    "type": "object",
    "properties": {
      "command":     { "type": "string", "description": "要执行的命令" },
      "timeout_sec": { "type": "integer", "description": "超时秒数，默认 30" }
    },
    "required": ["command"]
  }
}


模型返回工具调用时，你的代码就调用 run_terminal(...)，内部再走 llm_shell exec，把结果回灌给模型即可。

### 5.3 直接给 Agent 框架

- LangChain / LlamaIndex：把 run_terminal 包成 Tool。
- MCP (Model Context Protocol)：把它包成一个 MCP tools/call handler 即可，本程序就是后端。
- Aider / Open Interpreter 类项目：它们通常直接 subprocess，你也可以指向 llm_shell exec 换成“隔离执行”。

---

## 6. 关键设计点

| 需求 | 实现方式 |
|---|---|
| 保有大模型进程的本地终端能力 | 守护进程通过 openpty + setsid + TIOCSCTTY 让子进程拿到一个真实 pty |
| 命令可交互式（如 top、vim、python -i） | 子进程 stdio 都接到 pty slave；输出回传到客户端 |
| 超时可控 | poll 循环 + kill(-pid, SIGKILL) |
| 隔离 | Unix socket + chmod 0600，仅本用户可用；不监听 TCP |
| 审计 | 每条命令与结果都写到 --log 指定文件 |
| 输出上限 | MAX_OUTPUT=4 MB，超限截断提示 |
| 不影响系统 | 守护进程以你当前用户身份运行，不涉及提权 |

---

## 7. 进一步加固（强烈建议生产环境做）

### 7.1 用 bubblewrap 沙箱执行

把子进程 execl 前改成：

execl("/usr/bin/bwrap",
      "bwrap",
      "--ro-bind", "/usr", "/usr",
      "--ro-bind", "/bin", "/bin",
      "--ro-bind", "/lib", "/lib",
      "--ro-bind", "/lib64", "/lib64",
      "--dev", "/dev",
      "--proc", "/proc",
      "--tmpfs", "/tmp",
      "--bind", "/data/hello", "/work",   // 只允许写这一处
      "--chdir", "/work",
      "--unshare-all",
      "--die-with-parent",
      "bash", "-c", cmd.c_str(), (char*)nullptr);


### 7.2 加白名单 / 黑名单

在 handleClient() 里，执行前加一层过滤：例如禁止 rm -rf /、mkfs、dd of=/dev/*、curl 外发数据等；或反过来只允许 ls/grep/find/git/npm/pip/... 等前缀。

### 7.3 会话持久化

当前实现每次命令是一个新的 bash，环境变量、cd 不保留。如果需要“一个长期会话”（模型可以逐步 cd 再执行），把 openpty + fork 一次，之后所有命令都往同一个 pty 里写入、读回来即可——那样就需要处理“如何判断一条命令结束”（一般用哨兵字符串，如 echo __DONE_$?__）。

### 7.4 并发

现在是一次只处理一个连接。想并发就在 accept 后 fork() 一个子进程处理连接，父进程继续 accept。

---

## 8. 快速自检

cd /data/hello/os/shell
g++ -std=c++17 llm_shell.cpp -o llm_shell -lutil

# 终端 A
./llm_shell daemon --timeout 10

# 终端 B
./llm_shell exec "echo hello"
./llm_shell exec "python3 -c 'print(1+2)'"
./llm_shell exec "cat /data/hello/os/shell/llm_shell.cpp | head -3"
./llm_shell exec "sleep 60"          # 应在 10s 内被杀，exit=124/137

# 查看日志
cat /tmp/llm_shell.log

