// llm_shell.cpp —— 本地命令执行守护进程 + 客户端
//
// 架构：
//   大模型 / 调用方  --(Unix socket)-->  llm_shell daemon
//                                          |
//                                          +-- fork + openpty
//                                          +-- execl("/bin/bash","-c", cmd)
//                                          +-- 收集输出 / 超时终止
//
// 编译:
//   g++ -std=c++17 llm_shell.cpp -o llm_shell -lutil
//
// 用法:
//   llm_shell daemon [--sock PATH] [--log PATH] [--timeout SEC]
//   llm_shell exec   [--sock PATH] [--timeout SEC] "<command>"
//   llm_shell info
//
// 协议（长度前缀，网络字节序）:
//   Client -> Server:
//     u32 cmd_len | cmd[cmd_len] | i32 timeout_sec(0=默认, <0=不限)
//   Server -> Client:
//     u8  status (0=正常, 1=超时, 2=内部错误)
//     u32 exit_code
//     u32 output_len | output[output_len]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// ------------------ 常量 ------------------
static const char* DEFAULT_SOCK    = "/tmp/llm_shell.sock";
static const char* DEFAULT_LOG     = "/tmp/llm_shell.log";
static const int   DEFAULT_TIMEOUT = 30;              // 秒
static const size_t MAX_CMD        = 1024 * 1024;      // 1 MB
static const size_t MAX_OUTPUT     = 4 * 1024 * 1024;  // 4 MB

// ------------------ 工具 ------------------
static bool writeAll(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}
static bool readAll(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}
static bool writeU32(int fd, uint32_t v) {
    uint32_t n = htonl(v);
    return writeAll(fd, &n, 4);
}
static bool readU32(int fd, uint32_t& v) {
    uint32_t n;
    if (!readAll(fd, &n, 4)) return false;
    v = ntohl(n);
    return true;
}
static bool writeI32(int fd, int32_t v) {
    int32_t n = (int32_t)htonl((uint32_t)v);
    return writeAll(fd, &n, 4);
}
static bool readI32(int fd, int32_t& v) {
    int32_t n;
    if (!readAll(fd, &n, 4)) return false;
    v = (int32_t)ntohl((uint32_t)n);
    return true;
}

// ------------------ 日志 ------------------
static void logLine(const std::string& logPath, const std::string& line) {
    if (logPath.empty()) return;
    FILE* f = std::fopen(logPath.c_str(), "a");
    if (!f) return;
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    std::fprintf(f, "[%s] %s\n", ts, line.c_str());
    std::fclose(f);
}

// ------------------ 命令执行（PTY） ------------------
struct ExecResult {
    int         status    = 0;    // 0=正常 1=超时 2=内部错误
    int         exit_code = -1;
    std::string output;
};

static ExecResult runCommand(const std::string& cmd, int timeout_sec, size_t maxOut) {
    ExecResult r;

    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) < 0) {
        r.status = 2;
        r.output = std::string("openpty 失败: ") + std::strerror(errno) + "\n";
        return r;
    }

    pid_t pid = fork();
    if (pid < 0) {
        ::close(master); ::close(slave);
        r.status = 2;
        r.output = std::string("fork 失败: ") + std::strerror(errno) + "\n";
        return r;
    }

    if (pid == 0) {
        // ---- 子进程：成为会话首进程 + 控制终端 + 绑定 stdio ----
        ::close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) ::close(slave);

        // 让子进程默认没颜色/不等待分页，输出更稳定（可按需去掉）
        setenv("TERM", "xterm-256color", 1);
        setenv("PAGER", "cat", 1);
        setenv("GIT_PAGER", "cat", 1);
        setenv("LESS", "-FRX", 1);

        execl("/bin/bash", "bash", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }

    ::close(slave);

    // master 设为非阻塞
    int fl = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);

    std::time_t start = std::time(nullptr);
    std::time_t drainDeadline = 0;
    bool child_exited = false;
    bool timed_out   = false;
    int  child_status = 0;

    char buf[4096];
    while (true) {
        // 子进程状态
        if (!child_exited) {
            pid_t w = waitpid(pid, &child_status, WNOHANG);
            if (w == pid) {
                child_exited = true;
                drainDeadline = std::time(nullptr) + 1;  // 再读 1s 收尾
            }
        }

        // 超时处理
        if (!timed_out && timeout_sec > 0 &&
            std::difftime(std::time(nullptr), start) >= timeout_sec) {
            timed_out = true;
            r.status  = 1;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }

        if (child_exited && std::time(nullptr) >= drainDeadline) break;

        struct pollfd pfd;
        pfd.fd = master;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 100);
        if (pr < 0 && errno != EINTR) break;

        if (pr > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::read(master, buf, sizeof(buf));
            if (n > 0) {
                if (r.output.size() < maxOut) {
                    size_t take = std::min((size_t)n, maxOut - r.output.size());
                    r.output.append(buf, take);
                    if (take < (size_t)n)
                        r.output += "\n[输出被截断]\n";
                }
            } else if (n == 0 || (n < 0 && (errno == EIO || errno == EAGAIN))) {
                if (n == 0) break;
            } else if (errno != EINTR && errno != EAGAIN) {
                break;
            }
        }
    }

    if (!child_exited) {
        int st = 0;
        if (waitpid(pid, &st, 0) == pid) {
            child_status = st;
            child_exited = true;
        }
    }

    ::close(master);

    if (WIFEXITED(child_status))        r.exit_code = WEXITSTATUS(child_status);
    else if (WIFSIGNALED(child_status)) r.exit_code = 128 + WTERMSIG(child_status);
    else                                r.exit_code = -1;

    return r;
}

// ------------------ 服务端 ------------------
static std::string g_sockPath;

static void onSignal(int) {
    if (!g_sockPath.empty()) unlink(g_sockPath.c_str());
    _exit(0);
}

static void handleClient(int cli, const std::string& logPath, int defaultTimeout) {
    uint32_t cmdLen = 0;
    if (!readU32(cli, cmdLen) || cmdLen == 0 || cmdLen > MAX_CMD) return;

    std::string cmd(cmdLen, '\0');
    if (!readAll(cli, &cmd[0], cmdLen)) return;

    int32_t timeoutOverride = 0;
    if (!readI32(cli, timeoutOverride)) return;

    int timeout = (timeoutOverride == 0) ? defaultTimeout
                : (timeoutOverride  <  0) ? 0              // 不限
                : (int)timeoutOverride;

    logLine(logPath, "CMD: " + cmd);

    ExecResult r = runCommand(cmd, timeout, MAX_OUTPUT);

    // 回应
    uint8_t status = (uint8_t)r.status;
    writeAll(cli, &status, 1);
    writeU32(cli, (uint32_t)r.exit_code);
    writeU32(cli, (uint32_t)r.output.size());
    if (!r.output.empty())
        writeAll(cli, r.output.data(), r.output.size());

    char summary[256];
    std::snprintf(summary, sizeof(summary),
                  "EXIT: %d (status=%u, timeout=%d), out=%zu bytes",
                  r.exit_code, (unsigned)r.status, (int)(r.status == 1),
                  r.output.size());
    logLine(logPath, summary);
}

static int runDaemon(const std::string& sockPath, const std::string& logPath,
                     int defaultTimeout) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    g_sockPath = sockPath;
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { std::perror("socket"); return 1; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockPath.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "socket 路径过长\n");
        return 1;
    }
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    unlink(sockPath.c_str());
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    chmod(sockPath.c_str(), 0600);   // 仅当前用户可用
    if (listen(srv, 8) < 0) { std::perror("listen"); return 1; }

    std::printf("[llm_shell] listening on %s (default timeout=%ds)\n",
                sockPath.c_str(), defaultTimeout);
    std::printf("[llm_shell] log=%s\n", logPath.empty() ? "(off)" : logPath.c_str());
    std::fflush(stdout);

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            break;
        }
        handleClient(cli, logPath, defaultTimeout);
        ::close(cli);
    }

    ::close(srv);
    unlink(sockPath.c_str());
    return 0;
}

// ------------------ 客户端 ------------------
static int runClient(const std::string& sockPath, const std::string& command,
                     int timeoutSec /* 0=默认, <0=不限 */) {
    int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { std::perror("socket"); return 1; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::fprintf(stderr, "无法连接 %s: %s\n", sockPath.c_str(), std::strerror(errno));
        std::fprintf(stderr, "守护进程没启动？运行: llm_shell daemon\n");
        return 1;
    }

    if (command.size() > MAX_CMD) { std::fprintf(stderr, "命令过长\n"); return 1; }
    writeU32(s, (uint32_t)command.size());
    writeAll(s, command.data(), command.size());
    writeI32(s, (int32_t)timeoutSec);

    uint8_t  status = 0;
    uint32_t exitCode = 0, outLen = 0;
    if (!readAll(s, &status, 1))  { std::fprintf(stderr, "读状态失败\n"); return 1; }
    if (!readU32(s, exitCode))    { return 1; }
    if (!readU32(s, outLen))      { return 1; }
    if (outLen > MAX_OUTPUT + 1024) { std::fprintf(stderr, "输出异常大\n"); return 1; }

    std::string out(outLen, '\0');
    if (outLen > 0 && !readAll(s, &out[0], outLen)) { return 1; }
    ::close(s);

    std::fwrite(out.data(), 1, out.size(), stdout);
    if (!out.empty() && out.back() != '\n') std::fputc('\n', stdout);
    std::fflush(stdout);

    if (status == 1) {
        std::fprintf(stderr, "[llm_shell] 命令超时被终止\n");
        return 124;
    }
    if (status == 2) {
        std::fprintf(stderr, "[llm_shell] 内部错误\n");
        return 125;
    }
    return (int)exitCode;
}

// ------------------ info ------------------
static int cmdInfo(const std::string& sockPath) {
    struct stat st{};
    if (stat(sockPath.c_str(), &st) == 0) {
        std::printf("socket: %s (mode=%04o, uid=%d)\n",
                    sockPath.c_str(), st.st_mode & 07777, (int)st.st_uid);
    } else {
        std::printf("socket: %s 不存在\n", sockPath.c_str());
    }
    std::printf("uid=%d, euid=%d, shell=%s, tty=%s\n",
                (int)getuid(), (int)geteuid(),
                std::getenv("SHELL") ? std::getenv("SHELL") : "(unset)",
                isatty(STDOUT_FILENO) ? "yes" : "no");
    return 0;
}

// ------------------ main ------------------
static void usage() {
    std::printf(
        "用法:\n"
        "  llm_shell daemon [--sock PATH] [--log PATH] [--timeout SEC]\n"
        "  llm_shell exec   [--sock PATH] [--timeout SEC] \"<command>\"\n"
        "  llm_shell info   [--sock PATH]\n"
        "\n"
        "示例:\n"
        "  llm_shell daemon --timeout 30\n"
        "  llm_shell exec \"uname -a\"\n"
        "  llm_shell exec --timeout 5 \"sleep 30\"\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];

    if (cmd == "daemon") {
        std::string sock = DEFAULT_SOCK, log = DEFAULT_LOG;
        int timeout = DEFAULT_TIMEOUT;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--sock"    && i+1 < argc) sock    = argv[++i];
            else if (a == "--log"     && i+1 < argc) log     = argv[++i];
            else if (a == "--timeout" && i+1 < argc) timeout = std::atoi(argv[++i]);
            else if (a == "--no-log")                log.clear();
        }
        return runDaemon(sock, log, timeout);
    }

    if (cmd == "exec" || cmd == "run") {
        std::string sock = DEFAULT_SOCK;
        int timeoutSec = 0;
        int i = 2;
        for (; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--sock"    && i+1 < argc) sock       = argv[++i];
            else if (a == "--timeout" && i+1 < argc) timeoutSec = std::atoi(argv[++i]);
            else if (a == "--no-timeout")            timeoutSec = -1;
            else                                      break;
        }
        if (i >= argc) { usage(); return 1; }
        std::string command;
        for (; i < argc; ++i) {
            if (!command.empty()) command += ' ';
            command += argv[i];
        }
        return runClient(sock, command, timeoutSec);
    }

    if (cmd == "info") {
        std::string sock = DEFAULT_SOCK;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--sock" && i+1 < argc) sock = argv[++i];
        }
        return cmdInfo(sock);
    }

    usage();
    return 1;
}