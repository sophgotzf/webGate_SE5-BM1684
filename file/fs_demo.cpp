#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <ctime>

namespace fs = std::filesystem;

// ---------- 工具 ----------
void printUsage() {
    std::cout <<
        "用法:\n"
        "  fs_demo list  <dir>\n"
        "  fs_demo read  <file>\n"
        "  fs_demo write <file> <content>\n"
        "  fs_demo gen   <template> <output> [KEY=VALUE ...]\n"
        "  fs_demo scan  <dir> [--ext .cpp,.h] [--depth N]\n"
        "\n"
        "gen 的内置变量(模板中用 {{NAME}} 引用):\n"
        "  {{__FILENAME__}}  {{__DATE__}}  {{__DATETIME__}}  {{__YEAR__}}\n";
}

std::string nowStr(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tmv);
    return buf;
}

// ---------- 原有功能 ----------
void listDir(const std::string& path) {
    std::cout << "==== 列出目录: " << path << " ====\n";
    if (!fs::exists(path)) { std::cout << "路径不存在: " << path << "\n"; return; }
    for (const auto& entry : fs::directory_iterator(path)) {
        auto t = entry.status().type();
        std::string type =
            (t == fs::file_type::directory) ? "[DIR ] " :
            (t == fs::file_type::regular)   ? "[FILE] " :
            (t == fs::file_type::symlink)   ? "[LINK] " : "[?   ] ";
        std::cout << type << entry.path().string();
        if (t == fs::file_type::regular)
            std::cout << "  (" << fs::file_size(entry.path()) << " bytes)";
        std::cout << "\n";
    }
}

void readFile(const std::string& path) {
    std::cout << "==== 读取文件: " << path << " ====\n";
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::cout << "无法打开文件: " << path << "\n"; return; }
    std::string line;
    while (std::getline(in, line)) std::cout << line << "\n";
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) { std::cerr << "无法写入文件: " << path << "\n"; return false; }
    out << content;
    std::cout << "已写入: " << path << " (" << content.size() << " bytes)\n";
    return true;
}

// ---------- 模板渲染 ----------
std::string renderTemplate(const std::string& tpl,
                           const std::map<std::string, std::string>& vars) {
    std::string out;
    out.reserve(tpl.size());
    size_t i = 0;
    while (i < tpl.size()) {
        if (i + 1 < tpl.size() && tpl[i] == '{' && tpl[i + 1] == '{') {
            size_t end = tpl.find("}}", i + 2);
            if (end == std::string::npos) { out.append(tpl, i, std::string::npos); break; }
            std::string key = tpl.substr(i + 2, end - (i + 2));
            // 去掉首尾空白
            size_t b = 0, e = key.size();
            while (b < e && std::isspace(static_cast<unsigned char>(key[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(key[e - 1]))) --e;
            std::string k = key.substr(b, e - b);
            auto it = vars.find(k);
            if (it != vars.end()) out += it->second;
            else                  out += "{{" + key + "}}"; // 未提供则原样保留
            i = end + 2;
        } else {
            out += tpl[i++];
        }
    }
    return out;
}

void genFromTemplate(const std::string& tplPath,
                     const std::string& outPath,
                     const std::vector<std::pair<std::string, std::string>>& userVars) {
    std::ifstream in(tplPath, std::ios::binary);
    if (!in) { std::cerr << "无法打开模板: " << tplPath << "\n"; return; }

    std::stringstream ss;
    ss << in.rdbuf();
    std::string tpl = ss.str();

    std::map<std::string, std::string> vars;
    vars["__FILENAME__"] = fs::path(outPath).filename().string();
    vars["__DATE__"]     = nowStr("%Y-%m-%d");
    vars["__DATETIME__"] = nowStr("%Y-%m-%d %H:%M:%S");
    vars["__YEAR__"]     = nowStr("%Y");
    for (auto& kv : userVars) vars[kv.first] = kv.second;

    std::string content = renderTemplate(tpl, vars);
    writeFile(outPath, content);
}

// ---------- 递归扫描 ----------
struct ScanStats {
    size_t dirs    = 0;
    size_t files   = 0;
    size_t matched = 0;
    uintmax_t bytes = 0;
};

bool extMatch(const fs::path& p, const std::vector<std::string>& exts) {
    if (exts.empty()) return true;
    std::string e = p.extension().string();
    for (const auto& x : exts) if (e == x) return true;
    return false;
}

void scanRecursive(const fs::path& dir, int depth, int maxDepth,
                   const std::vector<std::string>& exts, ScanStats& st) {
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    try {
        for (const auto& entry :
             fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
            const fs::path& p = entry.path();
            auto t = entry.status().type();

            if (t == fs::file_type::directory) {
                ++st.dirs;
                std::cout << indent << "[DIR ] " << p.filename().string() << "/\n";
                if (maxDepth < 0 || depth + 1 <= maxDepth)
                    scanRecursive(p, depth + 1, maxDepth, exts, st);
            } else if (t == fs::file_type::regular) {
                ++st.files;
                std::error_code ec;
                uintmax_t sz = fs::file_size(p, ec);
                if (ec) sz = 0;
                st.bytes += sz;

                bool ok = extMatch(p, exts);
                if (ok) ++st.matched;
                if (ok || exts.empty()) {
                    std::cout << indent << "[FILE] " << p.filename().string()
                              << "  (" << sz << " bytes)\n";
                }
            } else if (t == fs::file_type::symlink) {
                std::cout << indent << "[LINK] " << p.filename().string() << "\n";
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << indent << "[!] " << e.what() << "\n";
    }
}

// ---------- main ----------
int main(int argc, char* argv[]) {
    if (argc < 2) { printUsage(); return 1; }
    std::string cmd = argv[1];

    if (cmd == "list") {
        listDir(argc > 2 ? argv[2] : ".");

    } else if (cmd == "read") {
        if (argc < 3) { printUsage(); return 1; }
        readFile(argv[2]);

    } else if (cmd == "write") {
        if (argc < 4) { printUsage(); return 1; }
        if (!writeFile(argv[2], argv[3])) return 2;

    } else if (cmd == "gen") {
        if (argc < 4) { printUsage(); return 1; }
        std::string tpl = argv[2], out = argv[3];
        std::vector<std::pair<std::string, std::string>> vars;
        for (int i = 4; i < argc; ++i) {
            std::string kv = argv[i];
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            vars.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        genFromTemplate(tpl, out, vars);

    } else if (cmd == "scan") {
        if (argc < 3) { printUsage(); return 1; }
        std::string root = argv[2];
        std::vector<std::string> exts;
        int maxDepth = -1;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--ext" && i + 1 < argc) {
                std::stringstream ss(argv[++i]);
                std::string tok;
                while (std::getline(ss, tok, ','))
                    if (!tok.empty()) exts.push_back(tok);
            } else if (a == "--depth" && i + 1 < argc) {
                maxDepth = std::stoi(argv[++i]);
            }
        }
        if (!fs::exists(root)) { std::cerr << "路径不存在: " << root << "\n"; return 1; }
        std::cout << "==== 递归扫描: " << root << " ====\n";
        ScanStats st;
        scanRecursive(root, 0, maxDepth, exts, st);
        std::cout << "---- 汇总 ----\n"
                  << "目录数: "   << st.dirs
                  << "  文件数: " << st.files
                  << "  匹配: "   << st.matched
                  << "  总大小: " << st.bytes << " bytes\n";

    } else {
        printUsage();
        return 1;
    }
    return 0;
}