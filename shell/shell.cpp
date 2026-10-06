#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>      // fork, execvp, getpid, getppid
#include <sys/wait.h>    // waitpid
#include <sys/types.h>
#include <pwd.h>         // getpwuid
#include <errno.h>

// ------------------------------------------------------------
// 用 fork + execvp 启动一个真正继承终端的交互式 shell
// ------------------------------------------------------------
static int spawnInteractiveShell(const char* shellPath) {
    std::cout << "[*] 准备启动交互式 shell: " << shellPath << "\n";
    std::cout << "[*] 输入 exit 或按 Ctrl-D 退出。\n";

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "fork 失败: " << std::strerror(errno) << "\n";
        return 1;
    }

    if (pid == 0) {
        // ---- 子进程：替换为 shell ----
        // 参数约定: argv[0] 是程序名，可用 -i 明确要求交互式
        char* argv[] = {
            const_cast<char*>(shellPath),
            const_cast<char*>("-i"),
            nullptr
        };
        execvp(shellPath, argv);
        // execvp 只有在失败时才返回
        std::cerr << "execvp 失败: " << std::strerror(errno) << "\n";
        _exit(127);
    }

    // ---- 父进程：等待子 shell 结束 ----
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::cerr << "waitpid 失败: " << std::strerror(errno) << "\n";
        return 1;
    }

    if (WIFEXITED(status)) {
        std::cout << "[*] shell 已退出，返回码 = " << WEXITSTATUS(status) << "\n";
    } else if (WIFSIGNALED(status)) {
        std::cout << "[*] shell 被信号终止，signal = " << WTERMSIG(status) << "\n";
    }
    return 0;
}

// ------------------------------------------------------------
// 用 system() 执行单条命令（一次性）
// ------------------------------------------------------------
static int runCommand(const std::string& cmd) {
    std::cout << "[*] 执行: " << cmd << "\n";
    int rc = std::system(cmd.c_str());
    if (rc == -1) {
        std::cerr << "system 调用失败: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (WIFEXITED(rc)) {
        std::cout << "[*] 命令返回码 = " << WEXITSTATUS(rc) << "\n";
    }
    return 0;
}

int main(int argc, char* argv[]) {
    // 1. 打印当前环境信息
    uid_t uid = getuid();
    struct passwd* pw = getpwuid(uid);
    std::cout << "==== 终端/环境信息 ====\n";
    std::cout << "PID        : " << getpid() << "\n";
    std::cout << "PPID       : " << getppid() << "\n";
    std::cout << "UID        : " << uid
              << " (" << (pw ? pw->pw_name : "?") << ")\n";
    std::cout << "isatty(0)  : " << (isatty(STDIN_FILENO)  ? "yes" : "no")  << "\n";
    std::cout << "isatty(1)  : " << (isatty(STDOUT_FILENO) ? "yes" : "no")  << "\n";
    const char* term = std::getenv("TERM");
    const char* sh   = std::getenv("SHELL");
    std::cout << "TERM       : " << (term ? term : "(unset)") << "\n";
    std::cout << "SHELL      : " << (sh   ? sh   : "(unset)") << "\n\n";

    // 2. 命令行用法:
    //    ./shell               -> 进入交互式 shell
    //    ./shell -c "cmd"      -> 执行单条命令
    if (argc >= 3 && std::string(argv[1]) == "-c") {
        std::string cmd;
        for (int i = 2; i < argc; ++i) {
            if (i > 2) cmd += ' ';
            cmd += argv[i];
        }
        return runCommand(cmd);
    }

    // 3. 选择 shell
    const char* shell = (sh && *sh) ? sh : "/bin/sh";

    // 4. 非交互环境（例如通过管道运行）下直接提示
    if (!isatty(STDIN_FILENO)) {
        std::cout << "[!] stdin 不是终端，可能无法进入交互模式。\n"
                  << "    可改用: ./shell -c \"ls -l\"\n";
    }

    return spawnInteractiveShell(shell);
}