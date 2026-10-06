// llm_web.cpp —— 给 llm_shell 加一个 Web 界面（浏览器→HTTP→unix socket→daemon）
//
// 编译：
//   g++ -std=c++17 -O2 llm_web.cpp -o llm_web
// 运行：
//   ./llm_web --port 8080 --sock /tmp/llm_shell.sock
//
// 只监听 127.0.0.1，不会暴露到外网。请勿改 --bind 0.0.0.0！

#include <poll.h>
#include <vector>
#include <cctype>
#include <iconv.h>
#include <fcntl.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

// v1.2.1 新增：文件系统能力（桥接 /data/hello/os/file 的 fs_demo）+ 会话内工具调用
// v1.3.0：文件沙箱默认根从 /data/hello 放开成 /（= 整机都能碰，也就是"可以越界"），
//         web 的 Files 面板上用勾选框显示当前范围。
// v1.3.3：Files 面板那个勾选框从"只读展示"变成真开关 —— 勾上 = 只允许操作工作区内的路径，
//         取消勾选 = 放开越界（等价 --fs-allow-outside）。状态存浏览器 localStorage，
//         跟着每个请求带 allow_outside（和 "root" 一样只影响这一次请求），不用重启服务。
// v1.3.4：Files 面板去掉"删除"功能 —— 面板只提供浏览（列目录/打开文件/新建/编辑）入口，
//         列表里不再有「删除」按钮，前端也不再发 action:'remove'。浏览范围照旧由沙箱
//         管着：勾上 = 只能浏览工作区内的文件或文件夹，越界连列目录/打开都拒绝。
// v1.3.5：沙箱范围改成严格「按请求」：每个请求都从 --fs-root / --fs-allow-outside 的
//         启动值重新算，再被请求里的 root / allow_outside 覆盖一次。以前只在
//         fork-per-connection 下才干净，--no-fork 时上一次请求带的范围会残留
//         （带了 allow_outside:true 之后，后面没带的请求会白捡一个放开状态）。
// v1.3.6：默认系统提示词统一成一句 "you are a concise(SE5), helpful assistant. 用中文回答"
//         （就是页面脚本里的 SYSTEM_PROMPT 常量；服务端还会在它后面追加工作区/工具说明，
//         见 injectToolPrompt，两段拼成最终发给模型的那一条 system 消息）。
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <sys/stat.h>

static std::string g_sockPath = "/tmp/llm_shell.sock";
static int         g_port     = 8080;
static std::string g_bindIp   = "127.0.0.1";

// ---- 文件操作沙箱 ----
//   v1.3.0：默认工作区从 /data/hello 放开成 /，也就是默认就能越界（整机随便碰）。
//   想收窄回某个目录：--fs-root PATH；想干脆关掉越界检查：--fs-allow-outside。
//   v1.3.3：网页 Files 面板上的勾选框是真开关，它把 allow_outside 跟着每次请求带过来
//          （见 applyScope），所以不用重启也能收紧/放开；--fs-allow-outside 只定默认值。
static std::string g_fsRoot         = "/";
static std::string g_fsRootCanon;            // 启动时算好，用于越界判断
static bool        g_fsAllowOutside = false; // 服务端默认值；请求带 allow_outside 可临时改
// v1.3.5：上面这两个的「启动值」。每个请求都从它重新开始算（见 resetScopeToBoot），
//         这样 root / allow_outside 的「只影响这一次请求」在 --no-fork 下也成立。
static std::string g_fsRootBoot;             // = 启动参数规范化后的 --fs-root
static std::string g_fsRootCanonBoot;        // 同上（越界判断用的那个）
static bool        g_fsAllowOutsideBoot = false;
static int         g_maxToolRounds  = 8;     // 一次对话最多几轮工具调用
static int         g_chatTimeout    = 180;   // Chat 一轮对话的时间预算（秒）；--chat-timeout 可改
// 「超时(s)」输入框与服务端共用的边界（前端夹一次、服务端再夹一次，不靠单边信任）
static const int   kChatTimeoutMin  = 5;
static const int   kChatTimeoutMax  = 300;

namespace fs = std::filesystem;

// ---------- 读写工具 ----------
static bool writeAll(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}
static bool readAll(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n) {
        ssize_t r = ::read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}
static bool writeU32(int fd, uint32_t v) {
    uint32_t n = htonl(v);  return writeAll(fd, &n, 4);
}
static bool readU32 (int fd, uint32_t& v) {
    uint32_t n;             if (!readAll(fd, &n, 4)) return false;
    v = ntohl(n);           return true;
}
static bool writeI32(int fd, int32_t v) {
    uint32_t n = htonl((uint32_t)v); return writeAll(fd, &n, 4);
}
[[maybe_unused]] static bool readI32 (int fd, int32_t& v) {
    uint32_t n;             if (!readAll(fd, &n, 4)) return false;
    v = (int32_t)ntohl(n);  return true;
}

// ---------- 编码兜底 ----------
// 命令输出很可能是 GBK 的（这个工程里的源文件/文件名就是 GBK），直接塞进 JSON 会让
// 浏览器 r.json() 解析失败，终端面板就只剩空白/乱码。所以这里统一转成 UTF-8。
//
// 做法：按“非 ASCII 连续段”分别判断，互不影响——
//   纯 ASCII        → 原样（ASCII 在两种编码里完全一样）
//   合法 UTF-8 段   → 原样
//   不像 UTF-8 的段 → 坏字节够多就按 GB18030 转码，否则只把坏字节替换成 U+FFFD
// 这样即使一段输出里 UTF-8 和 GBK 混着，也不会互相带坏。
// 本文件里所有文字都必须保存为 UTF-8（响应头声明的是 charset=utf-8）。

// p 开头是否是一个合法 UTF-8 字符？是则返回长度，否则返回 0
static size_t utf8CharLen(const char* p, size_t n) {
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80) return 1;
    int extra; unsigned int cp;
    if      ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
    else return 0;
    if (n < (size_t)extra + 1) return 0;
    for (int k = 1; k <= extra; ++k) {
        unsigned char cc = (unsigned char)p[k];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (extra == 1 && cp < 0x80u)       return 0;   // 过长编码
    if (extra == 2 && cp < 0x800u)      return 0;
    if (extra == 3 && cp < 0x10000u)    return 0;
    if (cp > 0x10FFFFu)                 return 0;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;   // 代理区
    return (size_t)extra + 1;
}

static bool isValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size(); ) {
        size_t l = utf8CharLen(s.data() + i, s.size() - i);
        if (!l) return false;
        i += l;
    }
    return true;
}

// 只把非法字节替换成 U+FFFD，保证结果是合法 UTF-8
static std::string sanitizeUtf8(const char* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ) {
        size_t l = utf8CharLen(p + i, n - i);
        if (l) { out.append(p + i, l); i += l; }
        else   { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out;
}

// 整段当 GB18030 转 UTF-8；碰到脏字节（不是干净的 GBK）就放弃，返回 false
static bool gbkToUtf8(const char* p, size_t n, std::string& out) {
    iconv_t cd = ::iconv_open("UTF-8", "GB18030");
    if (cd == (iconv_t)-1) return false;            // 系统没带 gconv 模块
    char* src  = const_cast<char*>(p);
    size_t left = n;
    char buf[4096];
    bool clean = true;
    while (left) {
        char* q = buf;
        size_t room = sizeof(buf);
        size_t r = ::iconv(cd, &src, &left, &q, &room);
        out.append(buf, sizeof(buf) - room);
        if (r != (size_t)-1) continue;
        if (errno == E2BIG) continue;               // 输出缓冲满了，接着转
        clean = false;                              // EILSEQ/EINVAL
        break;
    }
    ::iconv_close(cd);
    return clean;
}

static std::string convertSegment(const char* p, size_t n) {
    size_t bad = 0;
    for (size_t i = 0; i < n; ) {
        size_t l = utf8CharLen(p + i, n - i);
        if (l) i += l; else { ++bad; ++i; }
    }
    if (bad == 0) return std::string(p, n);         // 本来就是合法 UTF-8
    if (bad * 4 >= n) {                             // 坏字节 >= 25%：基本可以断定是 GBK
        std::string conv;
        if (gbkToUtf8(p, n, conv)) return conv;
    }
    return sanitizeUtf8(p, n);                      // 其余：只替换坏字节，别动好字符
}

// 对外的统一入口：保证返回合法 UTF-8
static std::string toUtf8(const std::string& in) {
    if (isValidUtf8(in)) return in;                 // 绝大多数情况：直接返回
    std::string out;
    out.reserve(in.size() + in.size() / 4);
    size_t i = 0;
    while (i < in.size()) {
        if ((unsigned char)in[i] < 0x80) { out += in[i]; ++i; continue; }
        size_t j = i;
        while (j < in.size() && (unsigned char)in[j] >= 0x80) ++j;
        out += convertSegment(in.data() + i, j - i);
        i = j;
    }
    return out;
}

// 反向：UTF-8 → GB18030（GBK 的超集）。用于把浏览器输入写成 GBK 文件/GBK 文件名。
static bool utf8ToGbk(const char* p, size_t n, std::string& out) {
    iconv_t cd = ::iconv_open("GB18030", "UTF-8");
    if (cd == (iconv_t)-1) return false;
    char* src  = const_cast<char*>(p);
    size_t left = n;
    char buf[4096];
    bool clean = true;
    while (left) {
        char* q = buf;
        size_t room = sizeof(buf);
        size_t r = ::iconv(cd, &src, &left, &q, &room);
        out.append(buf, sizeof(buf) - room);
        if (r != (size_t)-1) continue;
        if (errno == E2BIG) continue;
        clean = false;
        break;
    }
    ::iconv_close(cd);
    return clean;
}

static std::string toGbk(const std::string& in) {
    std::string out;
    if (!utf8ToGbk(in.data(), in.size(), out)) return in;   // 转不了就原样
    return out;
}

// ---------- 调 daemon ----------
struct ExecResult {
    bool ok = false;
    int  status = 0;
    int  exitCode = -1;
    std::string output;
    std::string error;
};

static ExecResult callDaemon(const std::string& command, int timeoutSec) {
    ExecResult r;
    int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { r.error = std::strerror(errno); return r; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (g_sockPath.size() >= sizeof(addr.sun_path)) {
        r.error = "socket path too long"; ::close(s); return r;
    }
    std::strncpy(addr.sun_path, g_sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        r.error = std::string("connect: ") + std::strerror(errno);
        ::close(s); return r;
    }

    if (!writeU32(s, (uint32_t)command.size()) ||
        !writeAll(s, command.data(), command.size()) ||
        !writeI32(s, (int32_t)timeoutSec)) {
        r.error = "write failed"; ::close(s); return r;
    }

    uint8_t  status8 = 0;
    uint32_t exitCode = 0, outLen = 0;
    if (!readAll(s, &status8, 1) ||
        !readU32(s, exitCode) ||
        !readU32(s, outLen)) {
        r.error = "read failed"; ::close(s); return r;
    }
    if (outLen > 64u * 1024 * 1024) {
        r.error = "output too large"; ::close(s); return r;
    }
    std::string out(outLen, '\0');
    if (outLen && !readAll(s, &out[0], outLen)) {
        r.error = "read body failed"; ::close(s); return r;
    }
    ::close(s);
    r.ok = true;
    r.status = status8;
    r.exitCode = (int)exitCode;
    r.output = toUtf8(out);
    return r;
}

// ---------- JSON ----------
static std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"':  o += "\\\""; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else          { o += (char)c; }
        }
    }
    return o;
}

// 只找 "key": "value" 形式（值必须是字符串）
static std::string jsonGetString(const std::string& body, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return {};
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p = body.find('"', p + 1);
    if (p == std::string::npos) return {};
    std::string out;
    for (size_t q = p + 1; q < body.size(); ++q) {
        char c = body[q];
        if (c == '\\' && q + 1 < body.size()) {
            char e = body[++q];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"';  break;
                case '/': out += '/';  break;
                default:  out += e;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

// 找 "key": <number>
static bool jsonGetInt(const std::string& body, const std::string& key, int& out) {
    std::string pat = "\"" + key + "\"";
    size_t p = body.find(pat);
    if (p == std::string::npos) return false;
    p = body.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    const char* s = body.c_str() + p + 1;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s) return false;
    out = (int)v;
    return true;
}

// ============================================================
//  极简 JSON（对象/数组/字符串/数字/布尔/null）
//  v1.2.1 起 chat 支持 function calling，必须能真正解析模型返回的
//  tool_calls（含 arguments 这种"字符串里套 JSON"），字符串搜索不够用了。
// ============================================================
static std::string numToStr(double v) {
    char b[64];
    if (v == (double)(long long)v && v > -1e15 && v < 1e15)
        std::snprintf(b, sizeof(b), "%lld", (long long)v);
    else
        std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}

struct Jv {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type        t   = NUL;
    bool        b   = false;
    double      num = 0;
    std::string s;
    std::vector<Jv>                        arr;
    std::vector<std::pair<std::string, Jv>> obj;   // 保序

    static Jv Str (const std::string& v) { Jv j; j.t = STR;  j.s   = v; return j; }
    static Jv Num (double v)             { Jv j; j.t = NUM;  j.num = v; return j; }
    static Jv Bool(bool v)               { Jv j; j.t = BOOL; j.b   = v; return j; }
    static Jv Arr ()                     { Jv j; j.t = ARR;  return j; }
    static Jv Obj ()                     { Jv j; j.t = OBJ;  return j; }

    bool isObj() const { return t == OBJ; }
    bool isArr() const { return t == ARR; }
    bool isStr() const { return t == STR; }

    const Jv* find(const std::string& k) const {
        if (t != OBJ) return nullptr;
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    void set (const std::string& k, const Jv& v) {
        if (t != OBJ) { t = OBJ; obj.clear(); }
        for (auto& kv : obj) if (kv.first == k) { kv.second = v; return; }
        obj.emplace_back(k, v);
    }
    void push(const Jv& v) { if (t != ARR) { t = ARR; arr.clear(); } arr.push_back(v); }

    std::string asStr() const {
        switch (t) {
            case STR:  return s;
            case NUM:  return numToStr(num);
            case BOOL: return b ? "true" : "false";
            default:   return std::string();
        }
    }
    double      asNum() const { return t == NUM ? num : (t == BOOL ? (b ? 1 : 0) : 0); }
    bool        asBool() const { return t == BOOL ? b : (t == NUM ? num != 0 : !s.empty()); }
    int         asInt() const { return (int)asNum(); }
};

static void appendUtf8Cp(std::string& o, unsigned int cp) {
    if      (cp < 0x80u)    o += (char)cp;
    else if (cp < 0x800u)   { o += (char)(0xC0u | (cp >> 6));  o += (char)(0x80u | (cp & 0x3Fu)); }
    else if (cp < 0x10000u) { o += (char)(0xE0u | (cp >> 12)); o += (char)(0x80u | ((cp >> 6) & 0x3Fu));
                              o += (char)(0x80u | (cp & 0x3Fu)); }
    else                    { o += (char)(0xF0u | (cp >> 18)); o += (char)(0x80u | ((cp >> 12) & 0x3Fu));
                              o += (char)(0x80u | ((cp >> 6) & 0x3Fu)); o += (char)(0x80u | (cp & 0x3Fu)); }
}

struct JsonParser {
    const char* p; const char* e; bool ok = true;
    JsonParser(const char* b, size_t n) : p(b), e(b + n) {}

    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    bool lit(const char* s) {
        size_t n = std::strlen(s);
        if ((size_t)(e - p) < n || std::strncmp(p, s, n) != 0) { ok = false; return false; }
        p += n; return true;
    }

    int hex4(unsigned int& v) {
        if (e - p < 4) { ok = false; return 0; }
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else { ok = false; return 0; }
        }
        return 1;
    }

    std::string parseStr() {
        std::string o;
        if (p >= e || *p != '"') { ok = false; return o; }
        ++p;
        while (p < e) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') { ++p; return o; }
            if (c == '\\') {
                ++p;
                if (p >= e) break;
                char x = *p++;
                switch (x) {
                    case 'n': o += '\n'; break;
                    case 't': o += '\t'; break;
                    case 'r': o += '\r'; break;
                    case 'b': o += '\b'; break;
                    case 'f': o += '\f'; break;
                    case '/': o += '/';  break;
                    case '\\': o += '\\'; break;
                    case '"': o += '"';  break;
                    case 'u': {
                        unsigned int cp = 0;
                        if (!hex4(cp)) return o;
                        if (cp >= 0xD800u && cp <= 0xDBFFu && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            const char* save = p;
                            p += 2;
                            unsigned int lo = 0;
                            if (!hex4(lo)) { p = save; }
                            else if (lo >= 0xDC00u && lo <= 0xDFFFu)
                                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            else p = save;
                        }
                        appendUtf8Cp(o, cp);
                        break;
                    }
                    default: o += x;
                }
            } else {
                o += (char)c;
                ++p;
            }
        }
        ok = false;
        return o;
    }

    Jv parseVal() {
        ws();
        Jv v;
        if (p >= e) { ok = false; return v; }
        char c = *p;
        if (c == '{') {
            ++p; v = Jv::Obj(); ws();
            if (p < e && *p == '}') { ++p; return v; }
            while (p < e) {
                ws();
                std::string k = parseStr();
                if (!ok) return v;
                ws();
                if (p >= e || *p != ':') { ok = false; return v; }
                ++p;
                Jv child = parseVal();
                if (!ok) return v;
                v.obj.emplace_back(k, child);
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return v; }
                ok = false; return v;
            }
            ok = false; return v;
        }
        if (c == '[') {
            ++p; v = Jv::Arr(); ws();
            if (p < e && *p == ']') { ++p; return v; }
            while (p < e) {
                Jv child = parseVal();
                if (!ok) return v;
                v.arr.push_back(child);
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return v; }
                ok = false; return v;
            }
            ok = false; return v;
        }
        if (c == '"') return Jv::Str(parseStr());
        if (c == 't') { if (lit("true"))  return Jv::Bool(true);  return v; }
        if (c == 'f') { if (lit("false")) return Jv::Bool(false); return v; }
        if (c == 'n') { if (lit("null"))  return v;                return v; }
        {   // number
            const char* s = p;
            if (p < e && (*p == '-' || *p == '+')) ++p;
            while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                             *p == '-' || *p == '+')) ++p;
            if (p == s) { ok = false; return v; }
            std::string tmp(s, (size_t)(p - s));
            v = Jv::Num(std::strtod(tmp.c_str(), nullptr));
            return v;
        }
    }
};

static bool jsonParse(const std::string& s, Jv& out) {
    JsonParser jp(s.data(), s.size());
    out = jp.parseVal();
    return jp.ok && out.t != Jv::NUL;
}

static void jsonDumpTo(const Jv& v, std::string& o) {
    switch (v.t) {
        case Jv::NUL:  o += "null"; break;
        case Jv::BOOL: o += v.b ? "true" : "false"; break;
        case Jv::NUM:  o += numToStr(v.num); break;
        case Jv::STR: {
            o += '"';
            for (unsigned char c : v.s) {
                switch (c) {
                    case '"':  o += "\\\""; break;
                    case '\\': o += "\\\\"; break;
                    case '\n': o += "\\n";  break;
                    case '\r': o += "\\r";  break;
                    case '\t': o += "\\t";  break;
                    case '\b': o += "\\b";  break;
                    case '\f': o += "\\f";  break;
                    default:
                        if (c < 0x20) { char bb[8]; std::snprintf(bb, sizeof(bb), "\\u%04x", c); o += bb; }
                        else          { o += (char)c; }
                }
            }
            o += '"';
            break;
        }
        case Jv::ARR: {
            o += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) { if (i) o += ','; jsonDumpTo(v.arr[i], o); }
            o += ']';
            break;
        }
        case Jv::OBJ: {
            o += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) o += ',';
                jsonDumpTo(Jv::Str(v.obj[i].first), o);
                o += ':';
                jsonDumpTo(v.obj[i].second, o);
            }
            o += '}';
            break;
        }
    }
}

static std::string jsonDump(const Jv& v) {
    std::string o; o.reserve(256);
    jsonDumpTo(v, o);
    return o;
}

static Jv jErr(const std::string& msg) {
    Jv o = Jv::Obj();
    o.set("ok",    Jv::Bool(false));
    o.set("error", Jv::Str(msg));
    return o;
}

// 从对象里取各种类型的值（带默认值）
static std::string jStr (const Jv& o, const char* k, const std::string& def = "") {
    const Jv* v = o.find(k);
    return (v && v->t != Jv::NUL) ? v->asStr() : def;
}
static int  jInt (const Jv& o, const char* k, int def)  { const Jv* v = o.find(k); return (v && v->t != Jv::NUL) ? v->asInt()  : def; }
static bool jBool(const Jv& o, const char* k, bool def) { const Jv* v = o.find(k); return (v && v->t != Jv::NUL) ? v->asBool() : def; }

// ---- 工具调用形态归一化（v1.2.1） ----------------------------------------
// 这道兼容层专门为"文件功能被 web 对话捕获"服务：不同网关/模型返回的
// tool call 长得不一样，服务端必须先认出来，否则模型说要建文件也不会落盘。
//   1) arguments 有些网关直接给成 JSON 对象（不是"字符串里套 JSON"）。
//      不归一化就会被当成"没有参数"执行 → 报 缺少 path；而且回灌给模型的
//      assistant 消息会变成 arguments:""，很多网关直接 400。
//   2) content 有些网关给成"分片数组"，纯 asStr() 会拿到空字符串。
//   3) 老式 function_call（单次、非 tool_calls）也得能认。
static std::string jArgStr(const Jv& fnObj) {
    const Jv* v = fnObj.find("arguments");
    if (!v || v->t == Jv::NUL) return "{}";
    if (v->t == Jv::STR)       return v->s.empty() ? std::string("{}") : v->s;
    if (v->t == Jv::OBJ || v->t == Jv::ARR) return jsonDump(*v);
    return v->asStr();          // 数字/布尔等其它形态
}

static std::string jContentStr(const Jv& msg) {
    const Jv* cv = msg.find("content");
    if (!cv || cv->t == Jv::NUL) return std::string();
    if (cv->t == Jv::STR) return cv->s;
    if (cv->t == Jv::ARR) {
        std::string out;
        for (const auto& part : cv->arr) {
            if (part.t == Jv::STR) { out += part.s; continue; }
            const Jv* tx = part.find("text");
            if (tx && tx->t == Jv::STR) out += tx->s;
        }
        return out;
    }
    return cv->asStr();
}

static std::string capStr(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    std::string o = s.substr(0, n);
    o += "\n…[已截断，原长 " + std::to_string(s.size()) + " 字节]";
    return o;
}

// ============================================================
//  文件系统模块（v1.2.1）
//  把 /data/hello/os/file/fs_demo 的能力（list / read / write / gen / scan）
//  直接接进 web，另外补上 mkdir / append / remove / move / copy / stat。
//  所有操作都限制在 --fs-root（默认 /，即整机）沙箱内；
//  要彻底关掉越界检查就启动时加 --fs-allow-outside。
// ============================================================

static std::string fmtTime(time_t t) {
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char b[64];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}

static std::string modeStr(mode_t m) {
    std::string s;
    s += S_ISDIR(m)  ? 'd' : (S_ISLNK(m) ? 'l' : (S_ISREG(m) ? '-' : '?'));
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        s += (m & (1u << (8 - i))) ? rwx[i] : '-';
    return s;
}

static std::string base64Encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned v = ((unsigned char)in[i] << 16);
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += "==";
    } else if (rem == 2) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8);
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

static bool withinRoot(const fs::path& p) {
    if (g_fsAllowOutside || g_fsRootCanon.empty()) return true;
    const std::string a = p.string();
    const std::string r = g_fsRootCanon;
    if (a == r) return true;
    // 关键：根就是 "/" 时不能再比 a[r.size()] == '/'（"/x" 的第 1 个字符是 'x'，
    // 老写法会把 /x 判成越界，等于整个沙箱全拒绝）。根是 / 就是全放行。
    if (r == "/") return true;
    return a.size() > r.size() && a.compare(0, r.size(), r) == 0 && a[r.size()] == '/';
}

// 把请求里的路径解析成沙箱内的绝对路径
//   mustExist=false 时用于"新建"，此时允许路径还不存在
//   顺带处理老工程里的 GBK 文件名：UTF-8 找不到就再试 GBK 的写法
static bool fsResolve(const std::string& in, bool mustExist, fs::path& out, std::string& err) {
    if (in.empty())                            { err = "缺少 path"; return false; }
    if (in.size() > 4096)                      { err = "path 过长"; return false; }
    if (in.find('\0') != std::string::npos)    { err = "path 含非法字符"; return false; }

    std::vector<std::string> cands;
    cands.push_back(in);
    { std::string g = toGbk(in); if (g != in) cands.push_back(g); }

    fs::path chosen;
    bool found = false;
    for (size_t i = 0; i < cands.size(); ++i) {
        fs::path raw(cands[i]);
        fs::path full = raw.is_absolute() ? raw : (fs::path(g_fsRoot) / raw);
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(full, ec);
        if (ec) canon = full.lexically_normal();
        if (i == 0) chosen = canon;
        std::error_code ec2;
        if (fs::exists(canon, ec2)) { chosen = canon; found = true; break; }
    }

    out = chosen;
    if (!withinRoot(out)) {
        err = std::string("越界：只允许操作 ") +
              (g_fsRootCanon.empty() ? g_fsRoot : g_fsRootCanon) +
              " 内的路径（要放开就加 --fs-allow-outside）";
        return false;
    }
    if (mustExist && !found) { err = "路径不存在: " + in; return false; }
    return true;
}

static std::string typeOf(const fs::path& p) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (ec) return "other";
    if (fs::is_symlink(st))     return "link";
    if (fs::is_directory(st))   return "dir";
    if (fs::is_regular_file(st))return "file";
    return "other";
}

static Jv entryJson(const fs::path& p) {
    Jv e = Jv::Obj();
    e.set("name", Jv::Str(toUtf8(p.filename().string())));
    e.set("path", Jv::Str(toUtf8(p.string())));
    std::string type = typeOf(p);
    e.set("type", Jv::Str(type));
    if (type == "link") {
        std::error_code ec;
        e.set("is_dir", Jv::Bool(fs::is_directory(p, ec)));
    }
    struct stat st{};
    if (::stat(p.c_str(), &st) == 0) {
        e.set("size",  Jv::Num((double)st.st_size));
        e.set("mode",  Jv::Str(modeStr(st.st_mode)));
        e.set("mtime", Jv::Str(fmtTime(st.st_mtime)));
    }
    return e;
}

// ---- list：等价 fs_demo list（多了 type/size/mtime，按"目录优先 + 名字"排序） ----
static Jv opList(const Jv& a) {
    fs::path p; std::string err;
    if (!fsResolve(jStr(a, "path", "."), true, p, err)) return jErr(err);
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return jErr("不是目录: " + jStr(a, "path", "."));

    std::vector<fs::path> items;
    std::error_code itEc;
    for (fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, itEc), end;
         !itEc && it != end; it.increment(itEc)) {
        items.push_back(it->path());
    }

    std::sort(items.begin(), items.end(), [](const fs::path& x, const fs::path& y) {
        bool dx = typeOf(x) == "dir", dy = typeOf(y) == "dir";
        if (dx != dy) return dx;
        return x.filename().string() < y.filename().string();
    });

    Jv arr = Jv::Arr();
    for (const auto& it : items) arr.push(entryJson(it));

    Jv r = Jv::Obj();
    r.set("ok",      Jv::Bool(true));
    r.set("action",  Jv::Str("list"));
    r.set("path",    Jv::Str(toUtf8(p.string())));
    r.set("count",   Jv::Num((double)arr.arr.size()));
    r.set("entries", arr);
    if (itEc) r.set("warning", Jv::Str(itEc.message()));
    return r;
}

// ---- read：等价 fs_demo read（自动识别 UTF-8 / GBK，二进制给 base64） ----
static Jv opRead(const Jv& a) {
    fs::path p; std::string err;
    if (!fsResolve(jStr(a, "path"), true, p, err)) return jErr(err);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return jErr("是目录，请用 list/scan: " + jStr(a, "path"));
    if (!fs::is_regular_file(p, ec)) return jErr("不是普通文件: " + jStr(a, "path"));

    size_t maxBytes = (size_t)jInt(a, "max_bytes", 256 * 1024);
    if (maxBytes < 1024)       maxBytes = 1024;
    if (maxBytes > 8 * 1024 * 1024) maxBytes = 8 * 1024 * 1024;

    std::ifstream in(p, std::ios::binary);
    if (!in) return jErr(std::string("打开失败: ") + std::strerror(errno));
    std::string data;
    data.resize(maxBytes);
    in.read(&data[0], (std::streamsize)maxBytes);
    data.resize((size_t)in.gcount());

    uintmax_t total = fs::file_size(p, ec);
    if (ec) total = data.size();

    bool binary = false;
    for (unsigned char c : data) if (c == 0) { binary = true; break; }

    Jv r = Jv::Obj();
    r.set("ok",        Jv::Bool(true));
    r.set("action",    Jv::Str("read"));
    r.set("path",      Jv::Str(toUtf8(p.string())));
    r.set("size",      Jv::Num((double)total));
    r.set("truncated", Jv::Bool((uintmax_t)data.size() < total));

    std::string charset = jStr(a, "charset", "auto");
    if (binary) {
        r.set("encoding", Jv::Str("binary"));
        r.set("base64",   Jv::Str(base64Encode(data)));
        r.set("content",  Jv::Str(""));
        r.set("hint",     Jv::Str("二进制文件，content 为空，base64 字段是原文"));
    } else if (charset == "gbk" || charset == "gb18030") {
        std::string conv;
        if (!gbkToUtf8(data.data(), data.size(), conv)) conv = sanitizeUtf8(data.data(), data.size());
        r.set("encoding", Jv::Str("gbk"));
        r.set("content",  Jv::Str(conv));
    } else if (charset == "utf-8") {
        r.set("encoding", Jv::Str("utf-8"));
        r.set("content",  Jv::Str(sanitizeUtf8(data.data(), data.size())));
    } else {
        r.set("encoding", Jv::Str(isValidUtf8(data) ? "utf-8" : "gbk"));
        r.set("content",  Jv::Str(toUtf8(data)));
    }
    return r;
}

// ---- write / append：等价 fs_demo write（多了 charset / eol / mkdirs） ----
static Jv opWrite(const Jv& a, bool append) {
    const Jv* c = a.find("content");
    if (!c) return jErr("缺少 content");
    std::string content = (c->t == Jv::STR) ? c->s : jsonDump(*c);

    fs::path p; std::string err;
    if (!fsResolve(jStr(a, "path"), false, p, err)) return jErr(err);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return jErr("目标已是目录: " + jStr(a, "path"));

    if (jBool(a, "mkdirs", true)) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) return jErr("创建父目录失败: " + ec.message());
    }

    // 换行风格：keep(默认) / lf / crlf —— 这个工程的老文件是 CRLF，新文件常要跟齐
    std::string eol = jStr(a, "eol", "keep");
    if (!append && eol != "keep") {
        std::string out;
        out.reserve(content.size() + content.size() / 16 + 1);
        for (size_t i = 0; i < content.size(); ++i) {
            char ch = content[i];
            if (ch == '\r') {
                if (i + 1 < content.size() && content[i + 1] == '\n') { ++i; }
                if (eol == "crlf") out += "\r\n"; else out += '\n';
            } else if (ch == '\n') {
                if (eol == "crlf") out += "\r\n"; else out += '\n';
            } else out += ch;
        }
        if (eol == "crlf" && !out.empty() && out.back() != '\n') out += "\r\n";
        content = out;
    }

    std::string charset = jStr(a, "charset", "utf-8");
    bool gbkOut = (charset == "gbk" || charset == "gb18030");
    if (gbkOut) content = toGbk(content);

    std::ios::openmode mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
    std::ofstream out(p, mode);
    if (!out) return jErr(std::string("打开失败: ") + std::strerror(errno));
    out.write(content.data(), (std::streamsize)content.size());
    out.close();
    if (!out) return jErr("写入失败（权限或磁盘？）");

    struct stat st{};
    Jv r = Jv::Obj();
    r.set("ok",      Jv::Bool(true));
    r.set("action",  Jv::Str(append ? "append" : "write"));
    r.set("path",    Jv::Str(toUtf8(p.string())));
    r.set("bytes",   Jv::Num((double)content.size()));
    r.set("charset", Jv::Str(gbkOut ? "gbk" : "utf-8"));
    r.set("eol",     Jv::Str(eol));
    if (::stat(p.c_str(), &st) == 0) r.set("size", Jv::Num((double)st.st_size));
    return r;
}

static Jv opMkdir(const Jv& a) {
    fs::path p; std::string err;
    if (!fsResolve(jStr(a, "path"), false, p, err)) return jErr(err);
    std::error_code ec;
    if (fs::exists(p, ec)) {
        Jv r = Jv::Obj();
        r.set("ok",      Jv::Bool(true));
        r.set("action",  Jv::Str("mkdir"));
        r.set("path",    Jv::Str(toUtf8(p.string())));
        r.set("existed", Jv::Bool(true));
        return r;
    }
    fs::create_directories(p, ec);
    if (ec) return jErr("创建失败: " + ec.message());
    Jv r = Jv::Obj();
    r.set("ok",      Jv::Bool(true));
    r.set("action",  Jv::Str("mkdir"));
    r.set("path",    Jv::Str(toUtf8(p.string())));
    r.set("existed", Jv::Bool(false));
    return r;
}

static Jv opRemove(const Jv& a) {
    fs::path p; std::string err;
    if (!fsResolve(jStr(a, "path"), true, p, err)) return jErr(err);
    if (!g_fsRootCanon.empty() && p.string() == g_fsRootCanon) return jErr("拒绝删除沙箱根目录");

    bool recursive = jBool(a, "recursive", false);
    std::error_code ec;
    uintmax_t n = 0;
    if (recursive) n = fs::remove_all(p, ec);
    else if (fs::remove(p, ec)) n = 1;
    if (ec) return jErr("删除失败: " + ec.message() + (fs::is_directory(p, ec) ? "（目录请带 recursive:true）" : ""));

    Jv r = Jv::Obj();
    r.set("ok",      Jv::Bool(true));
    r.set("action",  Jv::Str("remove"));
    r.set("path",    Jv::Str(toUtf8(p.string())));
    r.set("removed", Jv::Num((double)n));
    return r;
}

static Jv opMoveCopy(const Jv& a, bool isCopy) {
    fs::path src, dst; std::string err;
    if (!fsResolve(jStr(a, "path", jStr(a, "from")), true, src, err)) return jErr(err);
    if (!fsResolve(jStr(a, "to", jStr(a, "dst")), false, dst, err)) return jErr(err);

    std::error_code ec;
    if (jBool(a, "mkdirs", true)) {
        fs::create_directories(dst.parent_path(), ec);
        if (ec) return jErr("创建目标父目录失败: " + ec.message());
    }
    bool overwrite = jBool(a, "overwrite", false);
    if (fs::exists(dst, ec) && !overwrite)
        return jErr("目标已存在（要覆盖请带 overwrite:true）: " + jStr(a, "to", jStr(a, "dst")));

    if (isCopy) {
        auto opt = fs::copy_options::recursive;
        if (overwrite) opt |= fs::copy_options::overwrite_existing;
        fs::copy(src, dst, opt, ec);
        if (ec) return jErr("复制失败: " + ec.message());
    } else {
        fs::rename(src, dst, ec);
        if (ec) {   // 跨设备时 rename 会失败，退回"复制+删除"
            std::error_code ec2;
            auto opt = fs::copy_options::recursive | fs::copy_options::overwrite_existing;
            fs::copy(src, dst, opt, ec2);
            if (ec2) return jErr("移动失败: " + ec.message());
            fs::remove_all(src, ec2);
        }
    }
    Jv r = Jv::Obj();
    r.set("ok",     Jv::Bool(true));
    r.set("action", Jv::Str(isCopy ? "copy" : "move"));
    r.set("from",   Jv::Str(toUtf8(src.string())));
    r.set("to",     Jv::Str(toUtf8(dst.string())));
    return r;
}

static Jv opStat(const Jv& a) {
    fs::path p; std::string err;
    if (!fsResolve(jStr(a, "path"), true, p, err)) return jErr(err);
    Jv r = entryJson(p);
    r.set("ok",     Jv::Bool(true));
    r.set("action", Jv::Str("stat"));
    std::error_code ec;
    r.set("is_dir",      Jv::Bool(fs::is_directory(p, ec)));
    r.set("is_regular",  Jv::Bool(fs::is_regular_file(p, ec)));
    r.set("is_symlink",  Jv::Bool(fs::is_symlink(p, ec)));
    r.set("fs_root",     Jv::Str(g_fsRootCanon.empty() ? g_fsRoot : g_fsRootCanon));
    r.set("fs_allow_outside", Jv::Bool(g_fsAllowOutside));
    return r;
}

// ---- scan：等价 fs_demo scan（递归 + --ext 过滤 + --depth） ----
static Jv opScan(const Jv& a) {
    fs::path root; std::string err;
    if (!fsResolve(jStr(a, "path", "."), true, root, err)) return jErr(err);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return jErr("不是目录: " + jStr(a, "path", "."));

    std::vector<std::string> exts;
    {
        const Jv* e = a.find("ext");
        if (e && e->isArr())      for (const auto& x : e->arr) exts.push_back(x.asStr());
        else if (e && e->isStr()) {
            std::stringstream ss(e->s);
            std::string tok;
            while (std::getline(ss, tok, ',')) if (!tok.empty()) exts.push_back(tok);
        }
    }
    int maxDepth   = jInt(a, "depth", -1);
    size_t maxEnt  = (size_t)jInt(a, "max_entries", 2000);
    if (maxEnt < 10)     maxEnt = 10;
    if (maxEnt > 20000)  maxEnt = 20000;

    std::string tree;
    size_t dirs = 0, files = 0, matched = 0, shown = 0;
    uintmax_t bytes = 0;
    bool cut = false;

    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
        if (cut) return;
        std::string indent((size_t)depth * 2, ' ');
        std::error_code itEc;
        std::vector<fs::directory_entry> ents;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, itEc), end;
             !itEc && it != end; it.increment(itEc)) {
            ents.push_back(*it);
        }
        std::sort(ents.begin(), ents.end(), [](const fs::directory_entry& x, const fs::directory_entry& y) {
            bool dx = x.is_directory(), dy = y.is_directory();
            if (dx != dy) return dx;
            return x.path().filename().string() < y.path().filename().string();
        });
        for (const auto& en : ents) {
            if (shown >= maxEnt) { cut = true; return; }
            const fs::path& p = en.path();
            std::string type = typeOf(p);
            if (type == "dir") {
                ++dirs;
                tree += indent + "[DIR ] " + toUtf8(p.filename().string()) + "/\n";
                ++shown;
                if (maxDepth < 0 || depth + 1 <= maxDepth) walk(p, depth + 1);
            } else if (type == "file") {
                ++files;
                std::error_code ec2;
                uintmax_t sz = fs::file_size(p, ec2);
                if (ec2) sz = 0;
                bytes += sz;
                bool okExt = exts.empty();
                std::string e = p.extension().string();
                for (const auto& x : exts) if (e == x) { okExt = true; break; }
                if (okExt) {
                    ++matched;
                    tree += indent + "[FILE] " + toUtf8(p.filename().string()) + "  (" + std::to_string(sz) + " bytes)\n";
                    ++shown;
                }
            } else {
                tree += indent + "[LINK] " + toUtf8(p.filename().string()) + "\n";
                ++shown;
            }
        }
    };
    walk(root, 0);

    Jv r = Jv::Obj();
    r.set("ok",     Jv::Bool(true));
    r.set("action", Jv::Str("scan"));
    r.set("path",   Jv::Str(toUtf8(root.string())));
    r.set("tree",   Jv::Str(capStr(tree, 128 * 1024)));
    Jv st = Jv::Obj();
    st.set("dirs",    Jv::Num((double)dirs));
    st.set("files",   Jv::Num((double)files));
    st.set("matched", Jv::Num((double)matched));
    st.set("bytes",   Jv::Num((double)bytes));
    r.set("stats", st);
    if (cut) r.set("truncated", Jv::Bool(true));
    return r;
}

// ---- gen：等价 fs_demo gen（模板 {{VAR}} 渲染，内置 __FILENAME__ 等） ----
static Jv opGen(const Jv& a) {
    fs::path tpl, outp; std::string err;
    if (!fsResolve(jStr(a, "template", jStr(a, "path")), true, tpl, err)) return jErr(err);
    if (!fsResolve(jStr(a, "output", jStr(a, "out")), false, outp, err)) return jErr(err);

    std::ifstream in(tpl, std::ios::binary);
    if (!in) return jErr("无法读模板: " + jStr(a, "template"));
    std::stringstream ss;
    ss << in.rdbuf();
    std::string tplText = ss.str();
    if (!isValidUtf8(tplText)) tplText = toUtf8(tplText);

    std::map<std::string, std::string> vars;
    vars["__FILENAME__"] = outp.filename().string();
    vars["__DATE__"]     = fmtTime(std::time(nullptr)).substr(0, 10);
    vars["__DATETIME__"] = fmtTime(std::time(nullptr));
    vars["__YEAR__"]     = fmtTime(std::time(nullptr)).substr(0, 4);
    const Jv* uv = a.find("vars");
    if (uv && uv->isObj()) for (const auto& kv : uv->obj) vars[kv.first] = kv.second.asStr();

    std::string outText;
    outText.reserve(tplText.size());
    for (size_t i = 0; i < tplText.size(); ) {
        if (i + 1 < tplText.size() && tplText[i] == '{' && tplText[i + 1] == '{') {
            size_t end = tplText.find("}}", i + 2);
            if (end == std::string::npos) { outText.append(tplText, i, std::string::npos); break; }
            std::string key = tplText.substr(i + 2, end - (i + 2));
            size_t b = 0, e = key.size();
            while (b < e && std::isspace((unsigned char)key[b])) ++b;
            while (e > b && std::isspace((unsigned char)key[e - 1])) --e;
            std::string k = key.substr(b, e - b);
            auto it = vars.find(k);
            outText += (it != vars.end()) ? it->second : ("{{" + key + "}}");
            i = end + 2;
        } else {
            outText += tplText[i++];
        }
    }

    Jv wr = Jv::Obj();
    wr.set("path",    Jv::Str(toUtf8(outp.string())));
    wr.set("content", Jv::Str(outText));
    wr.set("charset", a.find("charset") ? *a.find("charset") : Jv::Str("utf-8"));
    wr.set("eol",     a.find("eol") ? *a.find("eol") : Jv::Str("keep"));
    wr.set("mkdirs",  Jv::Bool(true));
    Jv res = opWrite(wr, false);
    if (res.find("ok") && !res.find("ok")->asBool()) return res;
    res.set("action", Jv::Str("gen"));
    res.set("template", Jv::Str(toUtf8(tpl.string())));
    res.set("vars", Jv::Num((double)vars.size()));
    return res;
}

static Jv fsDispatch(const std::string& action, const Jv& a) {
    if (action == "list")   return opList(a);
    if (action == "read")   return opRead(a);
    if (action == "write")  return opWrite(a, false);
    if (action == "append") return opWrite(a, true);
    if (action == "mkdir")  return opMkdir(a);
    if (action == "remove" || action == "rm") return opRemove(a);
    if (action == "move" || action == "mv")   return opMoveCopy(a, false);
    if (action == "copy" || action == "cp")   return opMoveCopy(a, true);
    if (action == "stat")   return opStat(a);
    if (action == "scan")   return opScan(a);
    if (action == "gen")    return opGen(a);
    if (action.empty())     return jErr("缺少 action（可用: list/read/write/append/mkdir/remove/move/copy/stat/scan/gen）");
    return jErr("未知 action: " + action);
}

// ---------- HTTP ----------
static void sendResponse(int fd, int code, const char* reason,
                         const std::string& ctype, const std::string& body) {
    char h[512];
    int n = std::snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, reason, ctype.c_str(), body.size());
    if (n < 0) return;
    size_t len = (size_t)n;
    if (len >= sizeof(h)) len = sizeof(h) - 1;
    writeAll(fd, h, len);
    if (!body.empty()) writeAll(fd, body.data(), body.size());
}


//////
// ============================================================
//  LLM chat：fork/exec curl，把请求体通过 stdin 传给 curl，
//  同时读回 stdout/stderr，带超时。
//  不经过 shell，避免命令注入。
// ============================================================
static bool runCurl(const std::vector<std::string>& args,
                    const std::string& stdinData,
                    std::string& out,
                    std::string& err,
                    int timeoutSec)
{
    int pin[2], pout[2], perr[2];
    if (::pipe(pin) || ::pipe(pout) || ::pipe(perr)) return false;

    pid_t pid = fork();
    if (pid < 0) {
        ::close(pin[0]); ::close(pin[1]);
        ::close(pout[0]); ::close(pout[1]);
        ::close(perr[0]); ::close(perr[1]);
        return false;
    }

    if (pid == 0) {
        ::dup2(pin[0], 0);
        ::dup2(pout[1], 1);
        ::dup2(perr[1], 2);
        ::close(pin[0]); ::close(pin[1]);
        ::close(pout[0]); ::close(pout[1]);
        ::close(perr[0]); ::close(perr[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    ::close(pin[0]); ::close(pout[1]); ::close(perr[1]);

    // 写 stdin
    size_t pos = 0;
    while (pos < stdinData.size()) {
        ssize_t w = ::write(pin[1], stdinData.data() + pos, stdinData.size() - pos);
        if (w > 0) { pos += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        break;
    }
    ::close(pin[1]);

    int fl;
    fl = fcntl(pout[0], F_GETFL); fcntl(pout[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(perr[0], F_GETFL); fcntl(perr[0], F_SETFL, fl | O_NONBLOCK);

    bool outDone = false, errDone = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    char buf[8192];

    while (!(outDone && errDone)) {
        if (std::chrono::steady_clock::now() > deadline) {
            ::kill(pid, SIGKILL);
            break;
        }
        struct pollfd fds[2];
        int n = 0;
        if (!outDone) { fds[n].fd = pout[0]; fds[n].events = POLLIN; n++; }
        if (!errDone) { fds[n].fd = perr[0]; fds[n].events = POLLIN; n++; }
        if (n == 0) break;

        int pr = ::poll(fds, n, 100);
        if (pr < 0 && errno != EINTR) break;

        int idx = 0;
        if (!outDone) {
            if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r = ::read(pout[0], buf, sizeof(buf));
                if (r > 0) out.append(buf, (size_t)r);
                else if (r == 0) outDone = true;
                else if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) outDone = true;
            }
            idx++;
        }
        if (!errDone) {
            if (fds[idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r = ::read(perr[0], buf, sizeof(buf));
                if (r > 0) err.append(buf, (size_t)r);
                else if (r == 0) errDone = true;
                else if (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK) errDone = true;
            }
        }
    }

    ::close(pout[0]);
    ::close(perr[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// 用 curl 调 OpenAI 兼容 /chat/completions
// 入参：model、apiKey、baseUrl、messagesJson（形如 [{"role":"user","content":"..."}]）
// 返回：resolved JSON 字符串，形如 {"http_code":200,"body":"..."} 或 {"error":"..."}
static std::string callLLM(const std::string& model,
                           const std::string& apiKey,
                           const std::string& baseUrl,
                           const std::string& messagesJson,
                           double temperature,
                           int    timeoutSec)
{
    if (model.empty() || apiKey.empty() || baseUrl.empty() || messagesJson.empty())
        return "{\"error\":\"缺少 model / api_key / base_url / messages\"}";
    if (model.size() > 200 || apiKey.size() > 4096 || baseUrl.size() > 1024)
        return "{\"error\":\"参数过长\"}";
    if (timeoutSec < 5)   timeoutSec = 5;
    if (timeoutSec > 300) timeoutSec = 300;

    // 归一化 base url
    std::string url = baseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";

    std::string body =
        std::string("{\"model\":\"") + jsonEscape(model) + "\","
        "\"messages\":" + messagesJson + ","
        "\"temperature\":" + std::to_string(temperature) + "}";

    std::vector<std::string> args = {
        "curl", "-sS", "--max-time", std::to_string(timeoutSec),
        "-w", "\n__HTTP__%{http_code}",
        "-X", "POST", url,
        "-H", "Content-Type: application/json",
        "-H", "Authorization: Bearer " + apiKey,
        "-H", "Expect:",
        "--data-binary", "@-"
    };

    std::string out, err;
    bool ran = runCurl(args, body, out, err, timeoutSec + 5);

    int code = 0;
    std::string raw = toUtf8(out);
    size_t tag = raw.rfind("__HTTP__");
    if (tag != std::string::npos) {
        std::string codeStr = raw.substr(tag + 8);
        raw = raw.substr(0, tag);
        while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
        try { code = std::stoi(codeStr); } catch (...) { code = 0; }
    }

    // ran==false = curl 没正常退出（超时被 kill / 连接被掐断）。
    // 注意：http_code 只有 100 时也别当成最终状态 —— 那只是 100 Continue 中间响应。
    if (!ran || code == 0 || code == 100) {
        std::string why = (!ran)
            ? ("本轮调用超时或中断（上限 " + std::to_string(timeoutSec) + "s）")
            : ("没有拿到最终响应（http_code=" + std::to_string(code) + "）");
        std::string se = toUtf8(err);
        for (char& c : se) if (c == '\n' || c == '\r') c = ' ';
        if (se.size() > 300) se = se.substr(0, 300);
        return std::string("{\"error\":\"") + jsonEscape(why + (se.empty() ? "" : ("：" + se))) + "\"}";
    }

    return std::string("{\"http_code\":") + std::to_string(code)
         + ",\"body\":\"" + jsonEscape(raw) + "\""
         + (err.empty() ? "" : ",\"stderr\":\"" + jsonEscape(toUtf8(err)) + "\"")
         + "}";
}
//////

// ============================================================
//  工具调用（function calling）
//  这一层就是 /data/hello/os/file（FS，fs_demo 的能力）与
//  /data/hello/os/shell（SHELL，llm_shell daemon）在 web chat 里的桥：
//  模型说话 → 服务端真正落盘/执行 → 结果回灌给模型 → 继续对话。
// ============================================================
struct LlmResp {
    bool        ok   = false;   // 传输层成功（curl 跑完了）
    int         http = 0;
    std::string raw;
    std::string err;
    Jv          json;
};

static LlmResp callLLMApi(const std::string& baseUrl, const std::string& apiKey,
                          const Jv& body, int timeoutSec) {
    LlmResp r;
    if (timeoutSec < 5)   timeoutSec = 5;
    if (timeoutSec > 300) timeoutSec = 300;

    std::string url = baseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";

    std::vector<std::string> args = {
        "curl", "-sS", "--max-time", std::to_string(timeoutSec),
        "-w", "\n__HTTP__%{http_code}",
        "-X", "POST", url,
        "-H", "Content-Type: application/json",
        "-H", "Authorization: Bearer " + apiKey,
        "-H", "Expect:",
        "--data-binary", "@-"
    };

    std::string out, err;
    bool ran = runCurl(args, jsonDump(body), out, err, timeoutSec + 5);

    std::string raw = toUtf8(out);
    size_t tag = raw.rfind("__HTTP__");
    if (tag != std::string::npos) {
        std::string codeStr = raw.substr(tag + 8);
        raw = raw.substr(0, tag);
        while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r')) raw.pop_back();
        try { r.http = std::stoi(codeStr); } catch (...) { r.http = 0; }
    }
    r.raw = raw;
    r.err = toUtf8(err);
    // ran==false 表示 curl 没正常退出：--max-time 到了、被上面 runCurl 的 deadline KILL、
    // 或者连接被上游掐断。这时 %{http_code} 往往停在 "Expect: 100-continue" 的中间响应 100 上，
    // 它不是最终状态码。早先只判 http==0，于是超时被当成 "HTTP 100" 直接甩给前端（复现：
    // 500KB 请求体 + --max-time 3 → curl exit 28，__HTTP__100），把真正原因盖住了。
    if (!ran || r.http == 100 || r.http == 0) {
        std::string why = (!ran)
            ? ("本轮 LLM 调用超时或中断（上限 " + std::to_string(timeoutSec) + "s）")
            : ("没有拿到最终响应（http_code=" + std::to_string(r.http) + "）");
        std::string se = r.err;
        for (char& c : se) if (c == '\n' || c == '\r') c = ' ';
        if (se.size() > 300) se = se.substr(0, 300);
        r.err = why + (se.empty() ? "" : ("：" + se));
        r.ok  = false;
        return r;
    }
    r.ok = true;
    jsonParse(raw, r.json);
    return r;
}

// 工具清单（OpenAI tools 格式）
static const char* TOOLS_JSON = R"TOOLS([
  {"type":"function","function":{
    "name":"fs_list","description":"列出目录内容（等价 fs_demo list）。path 可以是相对工作区根目录的相对路径或绝对路径。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string","description":"目录路径，例如 /data/hello/os/shell 或 os/shell"}},
      "required":["path"]}}},
  {"type":"function","function":{
    "name":"fs_read","description":"读取文本文件内容（等价 fs_demo read），自动识别 UTF-8 / GBK；二进制返回 base64。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string","description":"文件路径"},
      "charset":{"type":"string","description":"auto(默认)|utf-8|gbk，强制按某种编码解码"},
      "max_bytes":{"type":"integer","description":"最多读多少字节，默认 262144"}},
      "required":["path"]}}},
  {"type":"function","function":{
    "name":"fs_write","description":"新建或覆盖写文件（等价 fs_demo write）。父目录不存在会自动创建。这是“新建文件”的正规做法。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string","description":"文件路径"},
      "content":{"type":"string","description":"文件全文内容"},
      "charset":{"type":"string","description":"utf-8(默认)|gbk。本仓库部分老源文件是 GBK，需要跟齐时填 gbk"},
      "eol":{"type":"string","description":"keep(默认)|lf|crlf。本仓库老文件是 CRLF，新建同类文件建议 crlf"},
      "mkdirs":{"type":"boolean","description":"是否自动创建父目录，默认 true"}},
      "required":["path","content"]}}},
  {"type":"function","function":{
    "name":"fs_append","description":"在文件末尾追加内容，文件不存在则创建。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string"},
      "content":{"type":"string"},
      "charset":{"type":"string","description":"utf-8(默认)|gbk"}},
      "required":["path","content"]}}},
  {"type":"function","function":{
    "name":"fs_mkdir","description":"创建目录（可多级）。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string","description":"目录路径"}},
      "required":["path"]}}},
  {"type":"function","function":{
    "name":"fs_remove","description":"删除文件或目录。删目录必须 recursive=true。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string"},
      "recursive":{"type":"boolean","description":"是否递归删除目录，默认 false"}},
      "required":["path"]}}},
  {"type":"function","function":{
    "name":"fs_move","description":"移动或重命名（等价 mv）。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string","description":"源路径"},
      "to":{"type":"string","description":"目标路径"},
      "overwrite":{"type":"boolean","description":"目标存在时是否覆盖，默认 false"}},
      "required":["path","to"]}}},
  {"type":"function","function":{
    "name":"fs_copy","description":"复制文件或目录（等价 cp -r）。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string"},
      "to":{"type":"string"},
      "overwrite":{"type":"boolean"}},
      "required":["path","to"]}}},
  {"type":"function","function":{
    "name":"fs_scan","description":"递归扫描目录（等价 fs_demo scan），可按扩展名和深度过滤，用来先看清项目结构再动手。",
    "parameters":{"type":"object","properties":{
      "path":{"type":"string","description":"起始目录"},
      "ext":{"type":"string","description":"扩展名过滤，如 \".cpp,.h\"，不填则全部"},
      "depth":{"type":"integer","description":"最大深度，-1 表示不限"},
      "max_entries":{"type":"integer","description":"最多返回多少条，默认 2000"}},
      "required":["path"]}}},
  {"type":"function","function":{
    "name":"fs_gen","description":"按模板渲染生成文件（等价 fs_demo gen）：模板里 {{NAME}} 会被 vars 替换，内置 {{__FILENAME__}} {{__DATE__}} {{__DATETIME__}} {{__YEAR__}}。",
    "parameters":{"type":"object","properties":{
      "template":{"type":"string","description":"模板文件路径"},
      "output":{"type":"string","description":"输出文件路径"},
      "vars":{"type":"object","description":"自定义变量，如 {\"NAME\":\"llm\"}"},
      "charset":{"type":"string","description":"utf-8(默认)|gbk"},
      "eol":{"type":"string","description":"keep(默认)|lf|crlf"}},
      "required":["template","output"]}}},
  {"type":"function","function":{
    "name":"run_shell","description":"在本机执行一条 shell 命令（走 llm_shell daemon，带超时）。需要看编译结果、跑测试、git 状态时用它。",
    "parameters":{"type":"object","properties":{
      "command":{"type":"string","description":"要执行的命令"},
      "timeout":{"type":"integer","description":"超时秒数，默认 30，最大 300"}},
      "required":["command"]}}}
])TOOLS";

struct ToolCtx {
    bool allowFs    = false;
    bool allowShell = false;
    int  shellTimeout = 30;
};

static Jv dispatchTool(const std::string& name, const Jv& args, const ToolCtx& ctx) {
    if (name.rfind("fs_", 0) == 0) {
        if (!ctx.allowFs) return jErr("服务端没开文件工具（网页 Chat 里勾上“允许文件操作”）");
        return fsDispatch(name.substr(3), args);
    }
    if (name == "run_shell" || name == "shell" || name == "run_terminal" || name == "exec") {
        if (!ctx.allowShell) return jErr("服务端没开命令执行（网页 Chat 里勾上“允许执行命令”）");
        std::string cmd = jStr(args, "command");
        if (cmd.empty()) return jErr("缺少 command");
        int t = jInt(args, "timeout", ctx.shellTimeout);
        if (t < 1) t = 1;
        if (t > 300) t = 300;
        ExecResult r = callDaemon(cmd, t);
        if (!r.ok) return jErr(std::string("执行失败: ") + r.error + "（llm_shell daemon 在跑吗？）");
        Jv o = Jv::Obj();
        o.set("ok",        Jv::Bool(true));
        o.set("exit_code", Jv::Num(r.exitCode));
        o.set("status",    Jv::Num(r.status));
        o.set("output",    Jv::Str(capStr(r.output, 32 * 1024)));
        return o;
    }
    return jErr("未知工具: " + name);
}

struct ChatTools {
    bool fs       = false;
    bool shell    = false;
    int  maxRounds = 8;
};

struct AgentResult {
    bool        ok = false;
    int         http = 0;
    std::string content;
    std::string error;
    std::string raw;
    Jv          toolLog = Jv::Arr();
    int         rounds = 0;
    bool        hitLimit = false;
    int         budget = 0;      // 这一轮实际用的时间预算（秒），回包照实报
};

// 给 system prompt 补上"你在这个工作区里、工具怎么用"的说明
static void injectToolPrompt(Jv& messages, const ChatTools& ct) {
    if (!messages.isArr()) { messages = Jv::Arr(); }
    std::string tip = "你运行在一台 Linux 开发板的 Web 控制台里，可以直接操作文件。工作区根目录：" +
                      (g_fsRootCanon.empty() ? g_fsRoot : g_fsRootCanon) + "。";
    if (ct.fs)
        tip += "需要新建/修改/删除文件或目录时，必须调用 fs_write / fs_mkdir 等工具真正落盘，"
               "不要只回复“已创建”而不调用工具；动手前不确定就先 fs_read / fs_list / fs_scan 看一眼。";
    if (ct.shell)
        tip += "也可以用 run_shell 执行命令（比如 make、git status）。";
    tip += "工具返回 JSON，ok=false 表示失败，请按 error 修正后重试；不要编造工具执行结果。回答用中文，尽量简洁。";

    if (!messages.arr.empty() && jStr(messages.arr[0], "role") == "system") {
        std::string c = jStr(messages.arr[0], "content");
        messages.arr[0].set("content", Jv::Str(c + "\n\n" + tip));
    } else {
        Jv sys = Jv::Obj();
        sys.set("role",    Jv::Str("system"));
        sys.set("content", Jv::Str(tip));
        messages.arr.insert(messages.arr.begin(), sys);
    }
}

// 多轮：模型要工具 → 本地执行 → 结果回灌 → 再问模型
static AgentResult chatWithTools(const std::string& model, const std::string& apiKey,
                                 const std::string& baseUrl, Jv messages,
                                 double temperature, int timeoutSec, const ChatTools& ct) {
    AgentResult ar;
    ToolCtx ctx;
    ctx.allowFs    = ct.fs;
    ctx.allowShell = ct.shell;

    // 按开关裁剪工具清单
    Jv allTools;
    if (!jsonParse(TOOLS_JSON, allTools) || !allTools.isArr()) {
        ar.error = "内部错误：工具清单解析失败";
        return ar;
    }
    Jv tools = Jv::Arr();
    for (const auto& t : allTools.arr) {
        const Jv* fn = t.find("function");
        std::string nm = fn ? jStr(*fn, "name") : "";
        bool isShell = (nm == "run_shell");
        if (isShell  && !ct.shell) continue;
        if (!isShell && !ct.fs)    continue;
        tools.push(t);
    }
    if (tools.arr.empty()) { ar.error = "没有启用任何工具"; return ar; }

    injectToolPrompt(messages, ct);

    int    rounds   = 0;
    // 预算就是请求里那个值（handleHttp 已经夹到 kChatTimeoutMin..Max）。
    // 老代码这里有个"至少 30s"的地板：输入框写 5、实际按 30 跑、回包又报 5，三处对不上账。
    int    budget   = (timeoutSec < kChatTimeoutMin) ? kChatTimeoutMin
                    : (timeoutSec > kChatTimeoutMax ? kChatTimeoutMax : timeoutSec);
    ar.budget = budget;
    auto   tStart   = std::chrono::steady_clock::now();
    std::map<std::string, int> sigSeen;
    std::string lastContent;

    while (true) {
        // 时间预算
        auto elapsedS = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - tStart).count();
        int left = budget - (int)elapsedS;
        // 还能不能再起一轮：第一轮无论如何都试（预算 5s 也是用户自己设的合法值），
        // 之后要求剩下至少 15s —— 只剩几秒必然超时，别把已经拿到的答复换成一句错误。
        int minSlice = (rounds == 0) ? 1 : 15;
        if (left < minSlice) {
            ar.ok = true;
            ar.content = lastContent.empty() ? "（已达到本次对话的时间上限，工具调用先停在这里）" : lastContent;
            ar.hitLimit = true;
            break;
        }
        // 单轮 LLM 调用上限 = 剩余预算，不再额外压一个固定值：
        // 上行只有 ~70KB/s，1MB 上下文光上传就 15s，再压小上限只会把"还没轮到"误判成超时。
        int per = left;

        Jv body = Jv::Obj();
        body.set("model",       Jv::Str(model));
        body.set("messages",    messages);
        body.set("temperature", Jv::Num(temperature));
        body.set("tools",       tools);
        body.set("tool_choice", Jv::Str("auto"));
        body.set("stream",      Jv::Bool(false));

        LlmResp resp = callLLMApi(baseUrl, apiKey, body, per);
        ar.http = resp.http;
        ar.raw  = resp.raw;
        if (!resp.ok) { ar.error = resp.err; ar.ok = false; return ar; }
        if (resp.http != 200) {
            ar.error = "HTTP " + std::to_string(resp.http);
            ar.ok = false;
            return ar;
        }
        const Jv* choices = resp.json.find("choices");
        if (!choices || !choices->isArr() || choices->arr.empty())
            { ar.error = "响应里没有 choices"; return ar; }
        const Jv* msg = choices->arr[0].find("message");
        if (!msg || !msg->isObj())
            { ar.error = "响应里没有 message"; return ar; }

        std::string content = jContentStr(*msg);
        if (!content.empty()) lastContent = content;

        // 统一取出工具调用：优先 tool_calls；老式 function_call 也认
        Jv tcs = Jv::Arr();
        const Jv* tcPtr = msg->find("tool_calls");
        if (tcPtr && tcPtr->isArr() && !tcPtr->arr.empty()) {
            tcs = *tcPtr;
        } else {
            const Jv* fc = msg->find("function_call");
            if (fc && fc->isObj() && !jStr(*fc, "name").empty()) {
                Jv c = Jv::Obj();
                c.set("id",   Jv::Str("call_legacy_" + std::to_string(rounds + 1)));
                c.set("type", Jv::Str("function"));
                Jv f = Jv::Obj();
                f.set("name",      Jv::Str(jStr(*fc, "name")));
                f.set("arguments", Jv::Str(jArgStr(*fc)));
                c.set("function", f);
                tcs.push(c);
            }
        }

        if (tcs.arr.empty()) {
            ar.ok      = true;
            ar.content = content.empty() ? "（模型没有返回内容）" : content;
            ar.rounds  = rounds;
            return ar;
        }

        if (rounds >= ct.maxRounds) {
            ar.ok       = true;
            ar.hitLimit = true;
            ar.rounds   = rounds;
            ar.content  = content.empty()
                        ? ("（工具调用已达上限 " + std::to_string(ct.maxRounds) + " 轮，先停在这里）")
                        : content;
            return ar;
        }
        ++rounds;

        // 把 assistant 的 tool_calls 原样回灌（字段精简，避免网关挑剔）
        Jv am = Jv::Obj();
        am.set("role", Jv::Str("assistant"));
        if (content.empty()) am.set("content", Jv());
        else                 am.set("content", Jv::Str(content));
        Jv calls = Jv::Arr();
        std::vector<std::string> names, argsTexts;
        for (size_t i = 0; i < tcs.arr.size(); ++i) {
            const Jv& tc = tcs.arr[i];
            const Jv* fn = tc.find("function");
            std::string nm  = fn ? jStr(*fn, "name") : "";
            std::string ats = fn ? jArgStr(*fn) : "{}";
            std::string id  = jStr(tc, "id", "call_" + std::to_string(rounds) + "_" + std::to_string(i));
            Jv c = Jv::Obj();
            c.set("id",   Jv::Str(id));
            c.set("type", Jv::Str("function"));
            Jv f = Jv::Obj();
            f.set("name",      Jv::Str(nm));
            f.set("arguments", Jv::Str(ats));
            c.set("function", f);
            calls.push(c);
            names.push_back(nm);
            argsTexts.push_back(ats);
        }
        am.set("tool_calls", calls);
        messages.push(am);

        // 逐个执行
        for (size_t i = 0; i < calls.arr.size(); ++i) {
            std::string nm = names[i], ats = argsTexts[i];
            std::string id = jStr(calls.arr[i], "id");

            Jv args;
            bool argsOk = jsonParse(ats, args) && args.isObj();
            if (ats.empty()) { argsOk = true; args = Jv::Obj(); }
            // 有的模型会把参数再包一层字符串（"\"{...}\""），这里再解一次
            if (argsOk && args.isStr()) {
                Jv inner;
                if (jsonParse(args.s, inner) && inner.isObj()) args = inner;
            }

            std::string dupKey = nm + "|" + ats;
            int seen = ++sigSeen[dupKey];

            auto t0 = std::chrono::steady_clock::now();
            Jv result;
            if (!argsOk)       result = jErr("arguments 不是合法 JSON 对象: " + capStr(ats, 512));
            else if (seen > 2) result = jErr("同一调用已经重复 " + std::to_string(seen - 1) +
                                             " 次，判定为死循环，请换个做法");
            else               result = dispatchTool(nm, args, ctx);
            auto t1 = std::chrono::steady_clock::now();

            std::string resText = capStr(jsonDump(result), 32 * 1024);

            Jv tm = Jv::Obj();
            tm.set("role",         Jv::Str("tool"));
            tm.set("tool_call_id", Jv::Str(id));
            tm.set("content",      Jv::Str(resText));
            messages.push(tm);

            Jv log = Jv::Obj();
            log.set("round", Jv::Num((double)rounds));
            log.set("tool",  Jv::Str(nm));
            log.set("args",  Jv::Str(capStr(ats, 4096)));
            log.set("ok",    Jv::Bool(result.find("ok") ? result.find("ok")->asBool()
                                                       : !result.find("error")));
            if (result.find("error")) log.set("error", Jv::Str(capStr(result.find("error")->asStr(), 1024)));
            log.set("result", Jv::Str(capStr(resText, 8192)));
            log.set("ms",     Jv::Num((double)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
            ar.toolLog.push(log);
        }
    }
    ar.rounds = rounds;
    return ar;
}

// ---------- 前端页面 ----------
//////
static const char* HTML = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>llm_shell - Web Console</title>
<style>
  :root{
    --bg:#0d1117; --panel:#161b22; --border:#30363d;
    --fg:#c9d1d9; --dim:#8b949e; --accent:#58a6ff;
    --ok:#3fb950; --err:#f85149; --warn:#d29922;
  }
  *{box-sizing:border-box}
  html,body{height:100%}
  body{margin:0;background:var(--bg);color:var(--fg);
    font:13px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
    display:flex;flex-direction:column}
  header{padding:8px 14px;border-bottom:1px solid var(--border);
    display:flex;align-items:center;gap:14px;background:var(--panel)}
  header h1{margin:0;font-size:14px;font-weight:600}
  nav{display:flex;gap:6px}
  nav button{background:transparent;border:1px solid transparent;color:var(--dim);
    padding:5px 12px;border-radius:4px;cursor:pointer;font:inherit;font-size:13px}
  nav button.on{background:#21262d;color:var(--fg);border-color:var(--border)}
  #stat{margin-left:auto;font-size:12px;color:var(--dim)}
  #stat.on{color:var(--ok)} #stat.off{color:var(--err)}
  main{flex:1;display:flex;flex-direction:column;min-height:0}
  .panel{flex:1;display:flex;flex-direction:column;min-height:0}
  .hidden{display:none !important}

  /* ---- Terminal ---- */
  #term-out{flex:1;overflow-y:auto;padding:12px 16px;white-space:pre-wrap;
    word-break:break-word;line-height:1.5}
  .entry{margin-bottom:14px}
  .cmd{color:var(--accent);font-weight:600}
  .cmd::before{content:"$ ";color:var(--dim)}
  .meta{color:var(--dim);font-size:12px}
  .meta.ok{color:var(--ok)} .meta.err{color:var(--err)} .meta.warn{color:var(--warn)}
  .body{margin-top:4px}

  /* ---- Chat ---- */
  #chat-wrap{flex:1;display:flex;flex-direction:column;min-height:0}
  #chat-conf{padding:8px 12px;border-bottom:1px solid var(--border);
    display:flex;gap:8px;flex-wrap:wrap;align-items:center;background:var(--panel)}
  #chat-conf label{font-size:12px;color:var(--dim);margin-left:4px}
  #chat-conf input{background:var(--bg);border:1px solid var(--border);color:var(--fg);
    padding:6px 8px;border-radius:4px;font:inherit;font-size:12px;outline:none}
  #chat-conf input:focus{border-color:var(--accent)}
  #chat-conf input#model{width:180px}
  #chat-conf input#key{width:220px}
  #chat-conf input#base{width:260px}
  #chat-conf button{background:#30363d;color:var(--fg);border:0;padding:6px 12px;
    border-radius:4px;cursor:pointer;font:inherit;font-size:12px}
  #chat-conf button.pri{background:var(--accent);color:#fff;font-weight:600}
  #chat-msgs{flex:1;overflow-y:auto;padding:14px 18px;line-height:1.55}
  .msg{margin-bottom:14px;max-width:100%}
  .msg .who{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.05em}
  .msg.user .who{color:var(--accent)}
  .msg.assistant .who{color:var(--ok)}
  .msg .txt{margin-top:4px;white-space:pre-wrap;word-break:break-word}
  .msg.user .txt{color:#e6edf3}
  .msg.assistant .txt{color:var(--fg)}
  .msg .err{color:var(--err);white-space:pre-wrap}
  #chat-in{border-top:1px solid var(--border);background:var(--panel);
    padding:10px 12px;display:flex;gap:8px;align-items:flex-end}
  #chat-in textarea{flex:1;background:var(--bg);border:1px solid var(--border);
    color:var(--fg);padding:8px 10px;border-radius:4px;font:inherit;font-size:13px;
    resize:vertical;min-height:38px;max-height:180px;outline:none}
  #chat-in textarea:focus{border-color:var(--accent)}
  #chat-in button{background:var(--accent);color:#fff;border:0;padding:8px 18px;
    border-radius:4px;cursor:pointer;font:inherit;font-size:13px;font-weight:600}
  #chat-in button:disabled{opacity:.5;cursor:not-allowed}
  #chat-in button.alt{background:#30363d;font-weight:400}

  /* ---- Files ---- */
  #fs-bar{display:flex;gap:8px;align-items:center;padding:8px 12px;
    border-bottom:1px solid var(--border);background:var(--panel);flex-wrap:wrap}
  #fs-bar input[type=text]{flex:1;min-width:200px;background:var(--bg);border:1px solid var(--border);
    color:var(--fg);padding:6px 8px;border-radius:4px;font:inherit;font-size:12px;outline:none}
  #fs-bar input:focus{border-color:var(--accent)}
  #fs-bar button{background:#30363d;color:var(--fg);border:0;padding:6px 10px;
    border-radius:4px;cursor:pointer;font:inherit;font-size:12px}
  #fs-bar button.pri{background:var(--accent);color:#fff;font-weight:600}
  #fs-bar label#fs-scope{display:flex;align-items:center;gap:5px;font-size:12px;
    color:var(--dim);white-space:nowrap;border:1px solid var(--border);border-radius:4px;
    padding:5px 8px;background:var(--panel)}
  #fs-bar label#fs-scope{cursor:pointer}
  #fs-bar label#fs-scope input{margin:0;accent-color:var(--accent);cursor:pointer}
  #fs-bar label#fs-scope.open{color:var(--warn);border-color:var(--warn)}
  #fs-msg{font-size:12px;color:var(--dim)}
  #fs-msg.err{color:var(--err)} #fs-msg.ok{color:var(--ok)}
  #fs-main{flex:1;display:flex;min-height:0}
  #fs-list{flex:1;overflow-y:auto;min-width:0}
  #fs-list table{width:100%;border-collapse:collapse;font-size:12px}
  #fs-list th{text-align:left;color:var(--dim);font-weight:400;padding:5px 10px;
    border-bottom:1px solid var(--border);background:var(--panel);position:sticky;top:0}
  #fs-list td{padding:4px 10px;border-bottom:1px solid #1f242c;white-space:nowrap;
    overflow:hidden;text-overflow:ellipsis;max-width:520px}
  #fs-list tr:hover{background:#1b212a}
  #fs-list td.nm{cursor:pointer}
  #fs-list td.nm:hover{color:var(--accent)}
  #fs-list td.sz,#fs-list td.tm,#fs-list td.tp{color:var(--dim)}
  #fs-empty{padding:16px;color:var(--dim);font-size:12px}
  #fs-edit{flex:1;display:flex;flex-direction:column;min-height:0;min-width:0;
    border-left:1px solid var(--border)}
  #fs-edit-bar{display:flex;gap:8px;align-items:center;padding:8px 12px;
    border-bottom:1px solid var(--border);background:var(--panel)}
  #fs-edit-bar .nm{font-size:12px;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  #fs-edit-bar select{background:var(--bg);border:1px solid var(--border);color:var(--fg);
    padding:5px 6px;border-radius:4px;font:inherit;font-size:12px}
  #fs-edit-bar button{background:#30363d;color:var(--fg);border:0;padding:5px 12px;
    border-radius:4px;cursor:pointer;font:inherit;font-size:12px}
  #fs-edit-bar button.pri{background:var(--accent);color:#fff;font-weight:600}
  #fs-text{flex:1;width:100%;background:var(--bg);color:var(--fg);border:0;outline:none;
    padding:10px 12px;font:13px/1.5 ui-monospace,Consolas,monospace;resize:none;
    white-space:pre;overflow:auto}

  /* ---- Chat 里的工具调用轨迹 ---- */
  .tools{margin-top:6px;border-left:2px solid var(--border);padding-left:10px}
  .tools details{margin:4px 0}
  .tools summary{cursor:pointer;font-size:12px;color:var(--dim)}
  .tools summary.ok{color:var(--ok)} .tools summary.bad{color:var(--err)}
  .tools pre{margin:4px 0 0;padding:6px 8px;background:#0b0f14;border:1px solid var(--border);
    border-radius:4px;font-size:12px;color:var(--dim);overflow:auto;max-height:280px;
    white-space:pre-wrap;word-break:break-word}
  #chat-conf label.chk{display:flex;align-items:center;gap:4px;cursor:pointer}
  #chat-conf input#fs-root{width:190px}
  #chat-conf input#rounds{width:46px}
  #chat-conf input#llm-timeout{width:64px}

  /* 共用 */
  footer.bar{border-top:1px solid var(--border);padding:8px 12px;
    background:var(--panel);display:flex;gap:8px;align-items:center}
  footer.bar label{font-size:12px;color:var(--dim)}
  footer.bar input[type=number]{width:70px;background:var(--bg);border:1px solid var(--border);
    color:var(--fg);padding:4px 6px;border-radius:4px;font:inherit;font-size:12px}
  footer.bar input[type=text]{flex:1;background:var(--bg);border:1px solid var(--border);
    color:var(--fg);padding:8px 10px;border-radius:4px;font:inherit;font-size:13px;outline:none}
  footer.bar input[type=text]:focus{border-color:var(--accent)}
  footer.bar button{background:var(--accent);color:#fff;border:0;padding:8px 16px;
    border-radius:4px;cursor:pointer;font:inherit;font-size:13px;font-weight:600}
  footer.bar button.alt{background:#30363d;font-weight:400}
</style>
</head>
<body>
<header>
  <h1>llm_shell · Web Console</h1>
  <nav>
    <button id="tab-term" class="on">Terminal</button>
    <button id="tab-chat">Chat</button>
    <button id="tab-files">Files</button>
  </nav>
  <div id="stat" class="off">checking…</div>
</header>

<main>
  <!-- ===== Terminal ===== -->
  <section id="panel-term" class="panel">
    <div id="term-out"></div>
    <footer class="bar">
      <span style="color:var(--dim)">$</span>
      <input id="cmd" type="text" placeholder="输入命令，例如 uname -a，回车执行" autocomplete="off">
      <label>超时</label>
      <input id="to" type="number" value="30" min="1" max="300">
      <label>秒</label>
      <button id="run">执行</button>
      <button id="clr" class="alt" type="button">清屏</button>
    </footer>
  </section>

  <!-- ===== Chat ===== -->
  <section id="panel-chat" class="panel hidden">
    <div id="chat-wrap">
      <div id="chat-conf">
        <label>模型</label>
        <input id="model" type="text" placeholder="gpt-4o-mini">
        <label>Key</label>
        <input id="key" type="password" placeholder="sk-...">
        <label>Base URL</label>
        <input id="base" type="text" placeholder="https://api.openai.com/v1">
        <label>温度</label>
        <input id="temp" type="text" value="0.2" style="width:60px">
        <label class="chk"><input type="checkbox" id="use-fs" checked> 文件操作</label>
        <label class="chk"><input type="checkbox" id="use-shell"> 执行命令</label>
        <label>工作区</label>
        <input id="fs-root" type="text" placeholder="/">
        <label>工具轮数</label>
        <input id="rounds" type="text" value="8">
        <label>超时(s)</label>
        <input id="llm-timeout" type="number" value="180" min="5" max="300" step="1"
               title="一整轮对话的时间预算（含所有工具轮次），5~300 秒。
服务端也会夹一次：超出范围按边界算。跑长任务建议把长命令丢后台，再发一句「继续」看日志。">
        <button id="save-conf" class="pri">保存</button>
        <button id="clear-chat" type="button">清空对话</button>
      </div>
      <div id="chat-msgs"></div>
      <div id="chat-in">
        <textarea id="chat-text" rows="1" placeholder="说点什么…  Enter 发送，Shift+Enter 换行"></textarea>
        <button id="chat-send">发送</button>
      </div>
    </div>
  </section>

  <!-- ===== Files（桥接 /data/hello/os/file） =====
       v1.3.4：这一块只给"浏览"用（列目录/打开文件/新建/编辑），不再提供删除 -->
  <section id="panel-files" class="panel hidden">
    <div id="fs-bar">
      <input id="fs-path" type="text" placeholder="/">
      <button id="fs-go" class="pri">打开</button>
      <button id="fs-up">上级</button>
      <button id="fs-refresh">刷新</button>
      <button id="fs-newfile">新建文件</button>
      <button id="fs-newdir">新建目录</button>
      <!-- 沙箱范围（v1.3.3 起是真开关）：勾上 = 只允许操作当前工作区内的路径；
           取消勾选 = 放开越界（等价启动时的 --fs-allow-outside）。
           它和「工作区」一样是浏览器本地配置，跟着每个请求带 allow_outside，不用重启服务 -->
      <label id="fs-scope" title="勾上 = 只允许操作工作区内的路径；取消勾选 = 放开越界">
        <input type="checkbox" id="fs-inside" checked>
        <span id="fs-scope-txt">只允许操作 / 内的路径</span>
      </label>
      <span id="fs-msg"></span>
    </div>
    <div id="fs-main">
      <div id="fs-list"></div>
      <div id="fs-edit" class="hidden">
        <div id="fs-edit-bar">
          <span class="nm" id="fs-edit-name"></span>
          <select id="fs-charset">
            <option value="auto">自动编码</option>
            <option value="utf-8">UTF-8</option>
            <option value="gbk">GBK</option>
          </select>
          <select id="fs-eol">
            <option value="keep">保留换行</option>
            <option value="crlf">CRLF</option>
            <option value="lf">LF</option>
          </select>
          <button id="fs-save" class="pri">保存</button>
          <button id="fs-close">关闭</button>
        </div>
        <textarea id="fs-text" spellcheck="false"></textarea>
      </div>
    </div>
  </section>
</main>

<script>
(function(){
  // ---------- 通用 ----------
  const $ = id => document.getElementById(id);
  const statEl = $('stat');
  function setStatus(txt, cls){ statEl.textContent = txt; statEl.className = cls || ''; }

  // ---------- Tab 切换 ----------
  const tabTerm = $('tab-term'), tabChat = $('tab-chat'), tabFiles = $('tab-files');
  const panTerm = $('panel-term'), panChat = $('panel-chat'), panFiles = $('panel-files');
  function showTab(name){
    const t = name === 'term', c = name === 'chat', f = name === 'files';
    tabTerm.classList.toggle('on', t);
    tabChat.classList.toggle('on', c);
    tabFiles.classList.toggle('on', f);
    panTerm.classList.toggle('hidden', !t);
    panChat.classList.toggle('hidden', !c);
    panFiles.classList.toggle('hidden', !f);
    if (c) setTimeout(()=> $('chat-text').focus(), 0);
    if (f) fsRefresh();
  }
  tabTerm.addEventListener('click', ()=> showTab('term'));
  tabChat.addEventListener('click', ()=> showTab('chat'));
  tabFiles.addEventListener('click', ()=> showTab('files'));

  // ================= Terminal 模块 =================
  const outEl = $('term-out'), cmdEl = $('cmd'), toEl = $('to'),
        runBtn = $('run'), clrBtn = $('clr');
  const history = []; let hIdx = -1;

  function appendEntry(cmd, timeoutSec){
    const div = document.createElement('div');
    div.className = 'entry';
    div.innerHTML = '<div class="cmd"></div>' +
                    '<div class="meta">timeout=' + timeoutSec + 's · running…</div>' +
                    '<div class="body"></div>';
    div.querySelector('.cmd').textContent = cmd;
    outEl.appendChild(div);
    outEl.scrollTop = outEl.scrollHeight;
    return div;
  }

  async function runCommand(){
    const cmd = cmdEl.value.trim();
    if (!cmd) return;
    if (history[0] !== cmd) history.unshift(cmd);
    hIdx = -1;

    const t = Math.max(1, Math.min(300, parseInt(toEl.value)||30));
    const entry = appendEntry(cmd, t);
    const meta = entry.querySelector('.meta');
    const body = entry.querySelector('.body');

    runBtn.disabled = true; setStatus('running…','');
    try {
      const resp = await fetch('/api/exec', {
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body: JSON.stringify({command: cmd, timeout: t})
      });
      const data = await resp.json();
      if (data.error){
        meta.className = 'meta err';
        meta.textContent = 'error: ' + data.error;
        body.textContent = data.output || '';
      } else {
        const code = data.exit_code;
        meta.className = 'meta ' + (code === 0 ? 'ok' : (data.status === 1 ? 'warn' : 'err'));
        meta.textContent = 'exit=' + code + (data.status===1?' (timeout)':'') +
                           ' · ' + data.duration_ms + ' ms';
        body.textContent = data.output || '(无输出)';
      }
      setStatus('ready','on');
    } catch(e){
      meta.className = 'meta err';
      meta.textContent = 'network error: ' + e;
      setStatus('offline','off');
    } finally {
      runBtn.disabled = false;
      cmdEl.value = ''; cmdEl.focus();
      outEl.scrollTop = outEl.scrollHeight;
    }
  }

  runBtn.addEventListener('click', runCommand);
  clrBtn.addEventListener('click', ()=>{ outEl.innerHTML=''; });
  cmdEl.addEventListener('keydown', e=>{
    if (e.key === 'Enter'){ e.preventDefault(); runCommand(); }
    else if (e.key === 'ArrowUp'){ e.preventDefault();
      if (hIdx + 1 < history.length){ hIdx++; cmdEl.value = history[hIdx]; }
    } else if (e.key === 'ArrowDown'){ e.preventDefault();
      if (hIdx > 0){ hIdx--; cmdEl.value = history[hIdx]; }
      else { hIdx = -1; cmdEl.value = ''; }
    }
  });

  // ================= Chat 模块 =================
  const modelEl = $('model'), keyEl = $('key'), baseEl = $('base'), tempEl = $('temp');
  const timeoutEl = $('llm-timeout');

  // Files 面板那个「只允许操作 X 内的路径」勾选框的状态（v1.3.3 起是真开关）：
  //   true = 放开越界；false = 只允许工作区内；null = 本机还没存过 → 先跟服务端对齐
  let scopeOutside = null;

  // 「超时(s)」= 一整轮对话的时间预算（含所有工具轮次），不是单次 curl 的超时。
  // 允许范围与服务端 kChatTimeoutMin/Max 一致，服务端还会自己夹一遍。
  const TO_MIN = 5, TO_MAX = 300, TO_DEF = 180;
  function normTimeout(v){
    let n = parseInt(v, 10);
    if (!isFinite(n) || n <= 0) n = TO_DEF;
    return Math.max(TO_MIN, Math.min(TO_MAX, n));
  }
  const useFsEl = $('use-fs'), useShellEl = $('use-shell'),
        fsRootEl = $('fs-root'), roundsEl = $('rounds');
  const saveConf = $('save-conf'), clearChat = $('clear-chat');
  const msgsEl = $('chat-msgs'), textEl = $('chat-text'), sendBtn = $('chat-send');

  // 从 localStorage 加载配置（仅本地浏览器）
  const CFG_KEY = 'llm_web_chat_conf_v2';
  try {
    const c = JSON.parse(localStorage.getItem(CFG_KEY)
              || localStorage.getItem('llm_web_chat_conf_v1') || '{}');
    modelEl.value = c.model || 'gpt-4o-mini';
    keyEl.value   = c.key   || '';
    baseEl.value  = c.base  || 'https://api.openai.com/v1';
    tempEl.value  = String(c.temp == null ? 0.2 : c.temp);
    useFsEl.checked    = (c.fs === undefined) ? true  : !!c.fs;
    useShellEl.checked = !!c.shell;
    fsRootEl.value = c.root || '/';
    roundsEl.value = String(c.rounds || 8);
    timeoutEl.value = String(normTimeout(c.timeout == null ? TO_DEF : c.timeout));
    scopeOutside = (typeof c.outside === 'boolean') ? c.outside : null;
  } catch(e){}

  function saveConfNow(){
    // 超时超出范围就在输入框里就地夹紧（服务端也会夹，先把话说清楚）
    const rawTo = timeoutEl.value.trim();
    const to    = normTimeout(rawTo);
    timeoutEl.value = String(to);
    localStorage.setItem(CFG_KEY, JSON.stringify({
      model: modelEl.value.trim(),
      key:   keyEl.value.trim(),
      base:  baseEl.value.trim(),
      temp:  tempEl.value.trim(),
      fs:    useFsEl.checked,
      shell: useShellEl.checked,
      root:  fsRootEl.value.trim(),
      rounds: roundsEl.value.trim(),
      timeout: String(to),
      outside: scopeOutside
    }));
    if (rawTo !== '' && String(to) !== rawTo){
      setStatus('超时已夹到 ' + to + 's（允许 ' + TO_MIN + '~' + TO_MAX + 's）','off');
    } else {
      setStatus('配置已保存','on');
    }
    setTimeout(()=> setStatus('ready','on'), 1500);
  }
  saveConf.addEventListener('click', saveConfNow);
  [modelEl,keyEl,baseEl,tempEl,fsRootEl,roundsEl,timeoutEl].forEach(el=>{
    el.addEventListener('keydown', e=>{ if (e.key==='Enter'){ e.preventDefault(); saveConfNow(); }});
  });
  [useFsEl, useShellEl].forEach(el=> el.addEventListener('change', saveConfNow));

  // 会话历史（OpenAI messages 格式）
  let messages = [];
  // 默认 system prompt（v1.3.6：全项目统一成这一句）。
  //   想改造型/措辞就改这里；删掉这一行或置成空串 = 不带 system 消息，
  //   服务端（injectToolPrompt）会自己新建一条，里面至少写着工作区在哪。
  //   注意：服务端还会在后面追加工具说明，最后发给模型的是两段拼起来的一条 system。
  const SYSTEM_PROMPT = 'you are a concise(SE5), helpful assistant. 用中文回答';

  // 工具调用轨迹：每个 tool_log 条目一个可折叠块
  function renderToolLog(log){
    const box = document.createElement('div');
    box.className = 'tools';
    for (const t of log){
      const det = document.createElement('details');
      const sum = document.createElement('summary');
      const ok = (t.ok !== false);
      sum.className = ok ? 'ok' : 'bad';
      let name = t.tool || '?';
      let brief = '';
      try {
        const a = JSON.parse(t.args || '{}');
        brief = a.path || a.template || a.command || '';
        if (a.to) brief += ' → ' + a.to;
        if (a.output) brief += ' → ' + a.output;
      } catch(e){ brief = (t.args || '').slice(0, 60); }
      sum.textContent = (ok ? '✓ ' : '✗ ') + name + (brief ? ' · ' + brief : '')
                      + (t.ms!=null ? '  (' + t.ms + ' ms)' : '');
      det.appendChild(sum);
      const pre = document.createElement('pre');
      pre.textContent = '参数:\n' + (t.args || '') +
                        '\n返回:\n' + (t.result || t.error || '');
      det.appendChild(pre);
      if (!ok) det.open = true;
      box.appendChild(det);
    }
    return box;
  }

  function renderAll(){
    msgsEl.innerHTML = '';
    if (messages.length === 0){
      const d = document.createElement('div');
      d.style.color = 'var(--dim)';
      d.style.padding = '4px 2px';
      d.textContent = '还没有对话。先在顶部填写模型与 Key（勾上“文件操作”，模型就能真的新建/修改文件），再在下方提问。';
      msgsEl.appendChild(d);
      return;
    }
    for (const m of messages){
      if (m.role === 'system') continue;
      const d = document.createElement('div');
      d.className = 'msg ' + m.role;
      const who = m.role === 'user' ? '你' : (m.role === 'assistant' ? '模型' : m.role);
      d.innerHTML = '<div class="who"></div><div class="txt"></div>';
      d.querySelector('.who').textContent = who;
      d.querySelector('.txt').textContent = m.content;
      if (m.tool_log && m.tool_log.length) d.appendChild(renderToolLog(m.tool_log));
      msgsEl.appendChild(d);
    }
    msgsEl.scrollTop = msgsEl.scrollHeight;
  }
  renderAll();

  clearChat.addEventListener('click', ()=>{
    messages = [];
    renderAll();
  });

  async function sendChat(){
    const text = textEl.value.trim();
    if (!text) return;
    const model = modelEl.value.trim();
    const apiKey = keyEl.value.trim();
    const base   = baseEl.value.trim();
    const temp   = parseFloat(tempEl.value) || 0.2;
    const budget = normTimeout(timeoutEl.value);   // 本轮预算（秒），服务端还会再夹一次

    if (!model || !apiKey || !base){
      setStatus('请先填写模型 / Key / Base URL','off');
      return;
    }
    saveConfNow();

    const wantFs    = useFsEl.checked;
    const wantShell = useShellEl.checked;
    const root      = fsRootEl.value.trim() || '/data/hello';
    const rounds    = Math.max(1, Math.min(20, parseInt(roundsEl.value)||8));

    // 加入用户消息
    messages.push({role:'user', content: text});
    textEl.value = '';
    renderAll();

    // 构造请求消息（含 system 前缀）
    const outbound = [];
    if (SYSTEM_PROMPT) outbound.push({role:'system', content: SYSTEM_PROMPT});
    for (const m of messages) outbound.push({role:m.role, content:m.content});

    sendBtn.disabled = true;
    setStatus(((wantFs||wantShell) ? '模型思考 / 调工具中…' : '模型思考中…')
              + '（预算 ' + budget + 's）','');

    // 加一条占位 assistant 气泡
    const ph = document.createElement('div');
    ph.className = 'msg assistant';
    ph.innerHTML = '<div class="who">模型</div><div class="txt" style="color:var(--dim)">…</div>';
    msgsEl.appendChild(ph);
    msgsEl.scrollTop = msgsEl.scrollHeight;

    try{
      const resp = await fetch('/api/chat', {
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body: JSON.stringify({
          model: model,
          api_key: apiKey,
          base_url: base,
          temperature: temp,
          timeout: budget,
          // 关键：把 messages 序列化成"字符串"
          messages: JSON.stringify(outbound),
          // 桥接开关：开了就等于允许模型在你工作区里建文件/跑命令
          tools: {fs: wantFs, shell: wantShell},
          root: root,
          // 沙箱范围跟「工作区」一样是这次请求的语义（勾选框放开时不带越界限制）
          allow_outside: scopeOutside === null ? undefined : scopeWantsOutside(),
          max_rounds: rounds
        })
      });
      const data = await resp.json();
      ph.remove();

      // ---- 带工具的新返回格式 ----
      if (data && data.content !== undefined){
        const log = data.tool_log || [];
        let finalText = data.content || '';
        const eff = (data && typeof data.timeout === 'number' && data.timeout > 0)
                    ? data.timeout : budget;
        if (!data.ok){
          finalText = '[错误] ' + (data.error || ('HTTP ' + data.http_code));
          if (/超时|中断|timed out/i.test(String(data.error || ''))){
            finalText += '\n\n（本轮预算 ' + eff + 's 用完。两条路：把顶部「超时(s)」调大；'
                       + '或者让长任务 nohup 丢后台，再发一句「继续」看日志）';
          }
        } else if (data.error){
          finalText = '[错误] ' + data.error;
        } else if (data.hit_limit){
          finalText += '\n\n（工具调用已到上限，可能还没做完，可以再说一句“继续”）';
        }
        messages.push({role:'assistant', content: finalText, tool_log: log});
        renderAll();
        if (log.length && wantFs) fsRefresh();           // 让 Files 面板跟上
        setStatus(data.ok ? ('ready · 工具 ' + log.length + ' 次 · 预算 ' + eff + 's') : '调用失败',
                  data.ok? 'on':'off');
        return;
      }

      // ---- 兼容老返回格式 ----
      if (data.error){
        messages.push({role:'assistant', content: '[错误] ' + data.error});
        renderAll();
        setStatus('调用失败','off');
        return;
      }
      const http = data.http_code;
      let rawBody = data.body || '';
      let parsed = null;
      try { parsed = JSON.parse(rawBody); } catch(e){}

      if (http !== 200){
        const apiErr = (parsed && parsed.error && (parsed.error.message || parsed.error)) || rawBody;
        messages.push({role:'assistant', content: `[HTTP ${http}] ${apiErr}`});
        renderAll();
        setStatus('HTTP ' + http,'off');
        return;
      }
      const content = (parsed && parsed.choices && parsed.choices[0]
                       && parsed.choices[0].message
                       && parsed.choices[0].message.content)
                    || '[无内容]';
      messages.push({role:'assistant', content: content});
      renderAll();
      setStatus('ready','on');
    }catch(e){
      ph.remove();
      messages.push({role:'assistant', content:'[网络错误] ' + e});
      renderAll();
      setStatus('offline','off');
    } finally {
      sendBtn.disabled = false;
      textEl.focus();
    }
  }

  sendBtn.addEventListener('click', sendChat);
  textEl.addEventListener('keydown', e=>{
    if (e.key === 'Enter' && !e.shiftKey){
      e.preventDefault();
      sendChat();
    }
  });

  // ================= Files 模块（桥接 /data/hello/os/file 的 fs_demo） =================
  // v1.3.4：只提供"浏览"入口（列目录 / 打开文件 / 新建 / 编辑保存），没有删除。
  const fsPathEl = $('fs-path'), fsListEl = $('fs-list'), fsMsgEl = $('fs-msg');
  const fsEditEl = $('fs-edit'), fsTextEl = $('fs-text'), fsEditName = $('fs-edit-name');
  const fsCharsetEl = $('fs-charset'), fsEolEl = $('fs-eol');
  const scopeCb = $('fs-inside');       // 沙箱范围勾选框
  let fsCurPath  = (fsRootEl.value.trim() || '/');
  let fsOpenFile = null;

  function fsMsg(t, cls){ fsMsgEl.textContent = t || ''; fsMsgEl.className = cls || ''; }

  // 沙箱范围那个勾选框（v1.3.3 起是真开关）：
  //   勾上     = 只允许操作当前工作区内的路径（越界直接拒绝）
  //   取消勾选 = 放开越界，任何路径都放行（等价启动时的 --fs-allow-outside）
  // 它是浏览器本地配置：跟「工作区」「超时(s)」一起存 localStorage，
  // 跟着每个请求带 allow_outside（只影响这一次请求），所以不用重启服务。
  let scopeRoot = '/';                          // 文案里显示的工作区（来自最近一次响应）
  function scopeWantsOutside(){ return scopeOutside === true; }

  function renderScope(d){
    const cb = $('fs-inside'), txt = $('fs-scope-txt'), lbl = $('fs-scope');
    if (!cb || !txt || !lbl) return;
    if (d && d.fs_root)             scopeRoot = d.fs_root;
    else if (fsRootEl.value.trim()) scopeRoot = fsRootEl.value.trim();
    // 本机没存过配置 → 跟着服务端（--fs-allow-outside）走，第一次响应就对齐
    if (scopeOutside === null && d && d.fs_allow_outside !== undefined)
      scopeOutside = !!d.fs_allow_outside;
    const open = scopeWantsOutside();
    cb.checked = !open;
    txt.textContent = open ? '已放开越界限制（勾上可重新收紧）'
                           : '只允许操作 ' + scopeRoot + ' 内的路径';
    lbl.className = open ? 'open' : '';
    lbl.title = open ? '越界检查关着：任何路径都能操作，勾上勾选框 = 重新收紧'
                     : '越过 ' + scopeRoot + ' 的路径会被拒绝（取消勾选 = 放开越界，不用重启服务）';
  }

  async function fsApi(payload){
    // 「工作区」和那个勾选框都是"这一次请求"的语义，所以每个 /api/fs 请求都带上：
    //   root          = 这次操作允许的范围（面板上那句话：只允许操作 <工作区> 内的路径）
    //   allow_outside = 勾选框：勾上 false（收紧）、取消勾选 true（放开越界）
    // v1.3.4：以前只有"列目录"带 root，打开 / 保存 / 新建 都没带 —— 服务端 --fs-root
    //         比「工作区」宽的时候（默认就是 /），这几种操作能碰到工作区外面的文件，
    //         跟面板上写的范围对不上。现在统一都带，浏览也被同一个范围管着。
    if (payload.root === undefined)
      payload.root = fsRootEl.value.trim() || undefined;   // 空 = 不带，服务端的 --fs-root 说了算
    if (scopeOutside !== null && payload.allow_outside === undefined)
      payload.allow_outside = scopeWantsOutside();
    const resp = await fetch('/api/fs', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify(payload)
    });
    return await resp.json();
  }

  function humanSize(n){
    n = Number(n) || 0;
    if (n < 1024) return n + ' B';
    if (n < 1024*1024) return (n/1024).toFixed(1) + ' K';
    return (n/1024/1024).toFixed(1) + ' M';
  }

  async function fsRefresh(path){
    const p = path || fsPathEl.value.trim() || fsCurPath;
    fsMsg('加载中…');
    let d;
    try { d = await fsApi({action:'list', path:p}); }   // root / allow_outside 由 fsApi 统一带上
    catch(e){ fsMsg('网络错误: ' + e, 'err'); return; }
    renderScope(d);
    if (!d || d.ok === false){ fsMsg((d && d.error) ? d.error : '读取失败', 'err'); return; }
    fsCurPath = d.path || p;
    fsPathEl.value = fsCurPath;
    renderFsList(d.entries || []);
    fsMsg((d.count || 0) + ' 项', 'ok');
  }

  function renderFsList(entries){
    fsListEl.innerHTML = '';
    if (!entries.length){
      const d = document.createElement('div');
      d.id = 'fs-empty';
      d.textContent = '（空目录）';
      fsListEl.appendChild(d);
      return;
    }
    const table = document.createElement('table');
    table.innerHTML = '<thead><tr><th>名称</th><th>类型</th><th>大小</th><th>修改时间</th></tr></thead>';
    const tb = document.createElement('tbody');
    for (const e of entries){
      const isDir = (e.type === 'dir') || (e.type === 'link' && e.is_dir);
      const tr = document.createElement('tr');

      const tdN = document.createElement('td');
      tdN.className = 'nm';
      tdN.textContent = (e.type === 'dir' ? '📁 ' : (e.type === 'link' ? '🔗 ' : '📄 ')) + e.name;
      tdN.title = e.path;
      tdN.addEventListener('click', ()=> isDir ? fsRefresh(e.path) : fsOpen(e.path));
      tr.appendChild(tdN);

      const tdT = document.createElement('td'); tdT.className = 'tp'; tdT.textContent = e.type; tr.appendChild(tdT);
      const tdS = document.createElement('td'); tdS.className = 'sz';
      tdS.textContent = isDir ? '' : humanSize(e.size); tr.appendChild(tdS);
      const tdM = document.createElement('td'); tdM.className = 'tm'; tdM.textContent = e.mtime || ''; tr.appendChild(tdM);

      // v1.3.4：这里原来是每行右侧那个「删除」按钮（发的是 action = remove），按需求去掉了。
      //         file 配置区（Files 面板）现在只有浏览入口 —— 列目录、打开文件、新建、编辑；
      //         删不了任何东西。后端 /api/fs 的 remove 与 Chat 里模型的 fs_remove 仍在，
      //         见 readme §4 / §7（要不要一起禁掉另说）。
      tb.appendChild(tr);
    }
    table.appendChild(tb);
    fsListEl.appendChild(table);
  }

  async function fsOpen(p){
    const d = await fsApi({action:'read', path:p});
    if (d.ok === false){ fsMsg(d.error || '打开失败', 'err'); return; }
    if (d.encoding === 'binary'){ fsMsg('二进制文件，这里不显示（要看得用 Terminal 里的 xxd/hexdump）', 'err'); return; }
    fsOpenFile = d.path;
    fsEditName.textContent = d.path + (d.truncated ? '  ⚠️ 太长只读了一部分，保存会截断' : '');
    fsTextEl.value = d.content || '';
    fsCharsetEl.value = (d.encoding === 'gbk') ? 'gbk' : 'utf-8';   // 默认按原编码存回去
    fsEolEl.value = 'keep';
    fsEditEl.classList.remove('hidden');
    fsTextEl.focus();
  }

  async function fsSave(){
    if (!fsOpenFile) return;
    const r = await fsApi({
      action:'write', path: fsOpenFile, content: fsTextEl.value,
      charset: fsCharsetEl.value, eol: fsEolEl.value
    });
    if (r.ok === false) fsMsg(r.error || '保存失败', 'err');
    else { fsMsg('已保存 ' + r.bytes + ' 字节（' + r.charset + ' / ' + r.eol + '）', 'ok'); fsRefresh(); }
  }

  async function fsCreate(isDir){
    const name = prompt(isDir ? '新目录名（可多级，相对 ' + fsCurPath + '）:' : '新文件名（可带子目录，相对 ' + fsCurPath + '）:',
                        isDir ? 'newdir' : 'new.txt');
    if (!name) return;
    const p = fsCurPath.replace(/\/+$/, '') + '/' + name;
    const r = isDir
      ? await fsApi({action:'mkdir', path:p})
      : await fsApi({action:'write', path:p, content:'', charset:'utf-8', eol:'lf'});
    if (r.ok === false){ fsMsg(r.error || '创建失败', 'err'); return; }
    fsMsg('已新建 ' + p, 'ok');
    fsRefresh();
    if (!isDir) fsOpen(p);
  }

  $('fs-go').addEventListener('click', ()=> fsRefresh(fsPathEl.value));
  $('fs-refresh').addEventListener('click', ()=> fsRefresh());
  $('fs-up').addEventListener('click', ()=>{
    const p = fsCurPath.replace(/\/+$/, '');
    const up = p.replace(/\/[^\/]*$/, '');
    fsRefresh(up || '/');
  });
  $('fs-newfile').addEventListener('click', ()=> fsCreate(false));
  $('fs-newdir').addEventListener('click',  ()=> fsCreate(true));
  $('fs-save').addEventListener('click',    ()=> fsSave());
  $('fs-close').addEventListener('click',   ()=>{ fsEditEl.classList.add('hidden'); fsOpenFile = null; });
  fsPathEl.addEventListener('keydown', e=>{ if (e.key === 'Enter'){ e.preventDefault(); fsRefresh(); } });
  fsTextEl.addEventListener('keydown', e=>{
    if ((e.ctrlKey || e.metaKey) && e.key === 's'){ e.preventDefault(); fsSave(); }
  });

  // 沙箱范围勾选框 = 真开关：勾/取消勾立刻换范围，并记进本机配置
  scopeCb.addEventListener('change', ()=>{
    scopeOutside = !scopeCb.checked;      // 勾上 = 只允许工作区内（不放行越界）
    renderScope(null);                    // 先把文案/高亮画成新的
    saveConfNow();                        // 跟别的配置一起存 localStorage
    fsRefresh();                          // 按新范围重新读一次当前目录
  });

  // ================= 启动时探测 daemon =================
  fetch('/api/health').then(r=>r.json()).then(d=>{
    renderScope(d);
    // 「超时(s)」的默认值由服务端给（--chat-timeout）：本机没存过配置就跟着它走
    if (typeof d.chat_timeout === 'number' && !localStorage.getItem(CFG_KEY)){
      timeoutEl.value = String(normTimeout(d.chat_timeout));
    }
    if (d.ok) setStatus('connected · ' + (d.sock||''), 'on');
    else      setStatus('daemon down: ' + (d.error||'?'), 'off');
  }).catch(()=> setStatus('server offline','off'));
})();
</script>
</body>
</html>
)HTML";
//////

// ---------- 请求小工具 ----------
static std::string urlDecode(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(s[i+1]), l = hex(s[i+2]);
            if (h >= 0 && l >= 0) { o += (char)((h << 4) | l); i += 2; continue; }
        }
        if (s[i] == '+') o += ' ';
        else             o += s[i];
    }
    return o;
}

// GET /api/fs?action=list&path=... → {"action":..,"path":..}
static Jv parseQuery(const std::string& qs) {
    Jv o = Jv::Obj();
    size_t i = 0;
    while (i < qs.size()) {
        size_t amp = qs.find('&', i);
        std::string kv = (amp == std::string::npos) ? qs.substr(i) : qs.substr(i, amp - i);
        size_t eq = kv.find('=');
        if (eq != std::string::npos)
            o.set(urlDecode(kv.substr(0, eq)), Jv::Str(urlDecode(kv.substr(eq + 1))));
        else if (!kv.empty())
            o.set(urlDecode(kv), Jv::Str("1"));
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    return o;
}

// 把请求里的开关值解析成 bool：JSON 里给 true/false 或者 1/0 直接用；
// GET 查询串里一律是字符串，所以还得认这几种常见写法
static bool parseSwitch(const Jv& v, bool& out) {
    if (v.t == Jv::BOOL) { out = v.b;          return true; }
    if (v.t == Jv::NUM)  { out = (v.num != 0); return true; }
    if (v.t == Jv::STR) {
        std::string s;
        for (char c : v.s) s += (char)std::tolower((unsigned char)c);
        if (s == "1" || s == "true"  || s == "on"  || s == "yes" || s == "y") { out = true;  return true; }
        if (s == "0" || s == "false" || s == "off" || s == "no"  || s == "n") { out = false; return true; }
    }
    return false;
}

// 允许本次请求临时改沙箱范围（每次连接一个进程，改全局变量是安全的）：
//   "root"          —— 临时换工作区根（Files 面板的「工作区」输入框就是这么用的）
//   "allow_outside" —— 临时开关越界检查（Files 面板那个勾选框跟着每个请求带过来）
static bool applyScope(const Jv& a, std::string& err) {
    std::string root = jStr(a, "root", jStr(a, "fs_root"));
    if (!root.empty()) {
        if (root.size() > 4096 || root.find('\0') != std::string::npos) { err = "root 非法"; return false; }

        fs::path p(root);
        if (!p.is_absolute()) p = fs::path(g_fsRoot) / p;
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(p, ec);
        if (ec) canon = p.lexically_normal();
        if (!fs::is_directory(canon, ec)) { err = "root 不是存在的目录: " + root; return false; }

        std::string s = canon.string();
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        g_fsRoot      = s;
        g_fsRootCanon = s;
    }

    const Jv* ao = a.find("allow_outside");
    if (ao && ao->t != Jv::NUL) {
        bool outside = false;
        if (!parseSwitch(*ao, outside)) {
            err = "allow_outside 非法: " + ao->asStr() + "（只认 true/false、1/0）";
            return false;
        }
        g_fsAllowOutside = outside;
    }
    return true;
}

// 每个请求都从「启动参数定的范围」重新开始。
//   root / allow_outside 是「这一次请求」的语义（网页 Files 面板的「工作区」和那个勾选框
//   就是跟着每个请求带过来的）。默认 fork-per-connection 时子进程天然是干净的；
//   但 --no-fork 下是同一个进程连着处理所有连接，不重置就会残留上一次请求带的范围
//   —— 最坏的情况：上一次请求带了 allow_outside:true，下一条**没带**的请求会白捡一个
//   「越界检查关着」的范围，本该拒的路径就没拒（沙箱形同虚设）。
static void resetScopeToBoot(){
    g_fsRoot         = g_fsRootBoot;
    g_fsRootCanon    = g_fsRootCanonBoot;
    g_fsAllowOutside = g_fsAllowOutsideBoot;
}

// ---------- 处理一个 HTTP 连接 ----------
static void handleHttp(int cli){
    resetScopeToBoot();   // v1.3.5：这次请求的范围 = 启动默认，上一笔的残留不许带进来
    std::string buf; buf.reserve(4096);
    char tmp[4096];
    size_t he = std::string::npos;

    while (true){
        ssize_t n = ::read(cli, tmp, sizeof(tmp));
        if (n <= 0) return;
        buf.append(tmp, (size_t)n);
        he = buf.find("\r\n\r\n");
        if (he != std::string::npos) break;
        if (buf.size() > 64*1024){ 
            sendResponse(cli, 414, "URI Too Long", "text/plain", "header too large"); return;
        }
    }

    size_t le = buf.find("\r\n");
    std::string reqLine = buf.substr(0, le);
    size_t sp1 = reqLine.find(' ');
    size_t sp2 = (sp1==std::string::npos) ? std::string::npos : reqLine.find(' ', sp1+1);
    if (sp1==std::string::npos || sp2==std::string::npos){
        sendResponse(cli, 400, "Bad Request", "text/plain", "bad request"); return;
    }
    std::string method = reqLine.substr(0, sp1);
    std::string path   = reqLine.substr(sp1+1, sp2-sp1-1);
    // 拆掉 query string，方便下面按路由名比较
    std::string query;
    {
        size_t qm = path.find('?');
        if (qm != std::string::npos) { query = path.substr(qm + 1); path = path.substr(0, qm); }
    }

    // Content-Length
    size_t cl = 0;
    {
        std::string headers = buf.substr(le+2, he-le-2);
        std::string lower = headers;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        size_t p = lower.find("content-length:");
        if (p != std::string::npos){
            size_t e = lower.find("\r\n", p);
            std::string v = headers.substr(p+15, e-(p+15));
            cl = (size_t)std::strtoul(v.c_str(), nullptr, 10);
        }
    }

    std::string body;
    if (cl){
        if (cl > 8*1024*1024){
            sendResponse(cli, 413, "Payload Too Large", "text/plain", "too large"); return;
        }
        body = buf.substr(he + 4);
        while (body.size() < cl){
            ssize_t n = ::read(cli, tmp, sizeof(tmp));
            if (n <= 0) break;
            body.append(tmp, (size_t)n);
        }
        if (body.size() > cl) body.resize(cl);
    }

    if (method == "GET" && (path == "/" || path == "/index.html")){
        sendResponse(cli, 200, "OK", "text/html; charset=utf-8", HTML); return;
    }

    if (method == "GET" && path == "/api/health"){
        ExecResult p = callDaemon("true", 1);
        // 顺带把沙箱范围带出去，前端用它显示那个勾选框
        std::string scope = std::string("\"fs_root\":\"")
            + jsonEscape(g_fsRootCanon.empty() ? g_fsRoot : g_fsRootCanon)
            + "\",\"fs_allow_outside\":" + (g_fsAllowOutside ? "true" : "false")
            // 前端「超时(s)」的默认值由服务端说了算，省得两处各写一个数
            + ",\"chat_timeout\":"     + std::to_string(g_chatTimeout)
            + ",\"chat_timeout_max\":" + std::to_string(kChatTimeoutMax);
        std::string j = p.ok
            ? std::string("{\"ok\":true,\"sock\":\"") + jsonEscape(g_sockPath) + "\"," + scope + "}"
            : std::string("{\"ok\":false,\"error\":\"") + jsonEscape(p.error) + "\"," + scope + "}";
        sendResponse(cli, 200, "OK", "application/json; charset=utf-8", j); return;
    }

    if (method == "POST" && path == "/api/exec"){
        std::string command = jsonGetString(body, "command");
        if (command.empty()){
            sendResponse(cli, 400, "Bad Request", "application/json; charset=utf-8",
                         "{\"error\":\"missing command\"}"); return;
        }
        int timeout = 30;
        int tFromJson = 0;
        if (jsonGetInt(body, "timeout", tFromJson) && tFromJson > 0) timeout = tFromJson;
        if (timeout < 1)   timeout = 1;
        if (timeout > 300) timeout = 300;

        auto t0 = std::chrono::steady_clock::now();
        ExecResult r = callDaemon(command, timeout);
        auto t1 = std::chrono::steady_clock::now();
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::string j;
        if (!r.ok){
            j = "{\"error\":\"" + jsonEscape(r.error) + "\","
                "\"output\":\"" + jsonEscape("确保 llm_shell daemon 正在运行，socket=" + g_sockPath) + "\","
                "\"exit_code\":-1,\"status\":2,\"duration_ms\":" + std::to_string(ms) + "}";
        } else {
            j = "{\"exit_code\":" + std::to_string(r.exitCode)
              + ",\"status\":"   + std::to_string(r.status)
              + ",\"duration_ms\":" + std::to_string(ms)
              + ",\"output\":\"" + jsonEscape(r.output) + "\"}";
        }
        sendResponse(cli, 200, "OK", "application/json; charset=utf-8", j); return;
    }
    
    // ============================================================
    //  /api/fs —— 文件系统桥：网页 Files 面板 + 模型工具都走这里
    //   GET  /api/fs?action=list&path=/data/hello[&root=...][&allow_outside=1]
    //   POST /api/fs  {"action":"write","path":"...","content":"...","allow_outside":true}
    //   两者都可以额外带 allow_outside 临时放开越界检查（网页 Files 面板的勾选框就是这么做）
    // ============================================================
    if ((method == "POST" || method == "GET") && path == "/api/fs") {
        Jv in = Jv::Obj();
        if (method == "POST") {
            if (!body.empty() && !jsonParse(body, in)) {
                sendResponse(cli, 400, "Bad Request", "application/json; charset=utf-8",
                             jsonDump(jErr("请求体不是合法 JSON")));
                return;
            }
        } else {
            in = parseQuery(query);
        }
        if (!in.isObj()) in = Jv::Obj();

        std::string err;
        if (!applyScope(in, err)) {
            sendResponse(cli, 200, "OK", "application/json; charset=utf-8", jsonDump(jErr(err)));
            return;
        }
        Jv res = fsDispatch(jStr(in, "action"), in);
        // 顺便把当前根目录带上，前端好显示
        res.set("fs_root", Jv::Str(g_fsRootCanon.empty() ? g_fsRoot : g_fsRootCanon));
        res.set("fs_allow_outside", Jv::Bool(g_fsAllowOutside));
        sendResponse(cli, 200, "OK", "application/json; charset=utf-8", jsonDump(res));
        return;
    }

    // 工具清单（排查用：看看当前能开出哪些工具）
    if (method == "GET" && path == "/api/tools") {
        Jv allTools;
        std::string out = "[]";
        if (jsonParse(TOOLS_JSON, allTools) && allTools.isArr()) out = jsonDump(allTools);
        Jv o = Jv::Obj();
        o.set("ok",        Jv::Bool(true));
        o.set("fs_root",   Jv::Str(g_fsRootCanon.empty() ? g_fsRoot : g_fsRootCanon));
        o.set("fs_allow_outside", Jv::Bool(g_fsAllowOutside));
        o.set("max_rounds", Jv::Num((double)g_maxToolRounds));
        o.set("chat_timeout",     Jv::Num((double)g_chatTimeout));
        o.set("chat_timeout_max", Jv::Num((double)kChatTimeoutMax));
        o.set("tools",     allTools);
        sendResponse(cli, 200, "OK", "application/json; charset=utf-8", jsonDump(o));
        return;
    }

    if (method == "POST" && path == "/api/chat") {
        Jv in;
        bool parsed = jsonParse(body, in) && in.isObj();

        std::string model, apiKey, baseUrl, messagesStr;
        double temperature = 0.2;
        int    timeout     = g_chatTimeout;   // 前端没带 / 带了非法值时的兜底（--chat-timeout）
        if (parsed) {
            model       = jStr(in, "model");
            apiKey      = jStr(in, "api_key");
            baseUrl     = jStr(in, "base_url");
            messagesStr = jStr(in, "messages");
            const Jv* t = in.find("temperature");
            if (t && t->t != Jv::NUL) temperature = t->asNum();
            timeout = jInt(in, "timeout", g_chatTimeout);
        } else {   // 老客户端：字符串搜索兜底
            model       = jsonGetString(body, "model");
            apiKey      = jsonGetString(body, "api_key");
            baseUrl     = jsonGetString(body, "base_url");
            messagesStr = jsonGetString(body, "messages");
            size_t p = body.find("\"temperature\"");
            if (p != std::string::npos) {
                size_t c = body.find(':', p);
                if (c != std::string::npos) {
                    const char* s = body.c_str() + c + 1;
                    while (*s == ' ' || *s == '\t') ++s;
                    char* end = nullptr;
                    double v = std::strtod(s, &end);
                    if (end != s) temperature = v;
                }
            }
            int t = 0;
            if (jsonGetInt(body, "timeout", t) && t > 0) timeout = t;
        }

        if (model.empty() || apiKey.empty() || baseUrl.empty() || messagesStr.empty()) {
            sendResponse(cli, 200, "OK", "application/json; charset=utf-8",
                         jsonDump(jErr("缺少 model / api_key / base_url / messages")));
            return;
        }
        if (timeout < kChatTimeoutMin) timeout = kChatTimeoutMin;
        if (timeout > kChatTimeoutMax) timeout = kChatTimeoutMax;

        // 工具开关：兼容 tools:{fs,shell} 和 use_fs/use_shell 两种写法
        ChatTools ct;
        const Jv* tv = in.find("tools");
        if (tv && tv->isObj()) {
            ct.fs    = jBool(*tv, "fs",    jBool(*tv, "file", false));
            ct.shell = jBool(*tv, "shell", jBool(*tv, "exec", false));
        }
        if (parsed) {
            if (in.find("use_fs"))    ct.fs    = jBool(in, "use_fs",    ct.fs);
            if (in.find("use_shell")) ct.shell = jBool(in, "use_shell", ct.shell);
            ct.maxRounds = jInt(in, "max_rounds", g_maxToolRounds);
        }
        if (ct.maxRounds < 1)  ct.maxRounds = 1;
        if (ct.maxRounds > 20) ct.maxRounds = 20;

        std::string rootErr;
        if (!applyScope(in, rootErr)) {
            sendResponse(cli, 200, "OK", "application/json; charset=utf-8", jsonDump(jErr(rootErr)));
            return;
        }

        // ---- 走工具（agent）流程 ----
        if (ct.fs || ct.shell) {
            Jv messages;
            bool mok = false;
            const Jv* mv = in.find("messages");
            if (mv && mv->isArr())      { messages = *mv; mok = messages.isArr(); }
            else if (mv && mv->isStr()) mok = jsonParse(mv->s, messages) && messages.isArr();
            else                        mok = jsonParse(messagesStr, messages) && messages.isArr();

            if (!mok) {
                sendResponse(cli, 200, "OK", "application/json; charset=utf-8",
                             jsonDump(jErr("messages 不是合法 JSON 数组")));
                return;
            }

            AgentResult ar = chatWithTools(model, apiKey, baseUrl, messages,
                                           temperature, timeout, ct);

            Jv o = Jv::Obj();
            o.set("ok",        Jv::Bool(ar.ok));
            o.set("http_code", Jv::Num((double)ar.http));
            // 生效的时间预算（服务端可能夹过）+ 上限，前端拿它显示"预算 Xs"并对不上时提醒
            o.set("timeout",     Jv::Num((double)(ar.budget > 0 ? ar.budget : timeout)));
            o.set("timeout_max", Jv::Num((double)kChatTimeoutMax));
            o.set("content",   Jv::Str(ar.content));
            o.set("body",      Jv::Str(ar.raw));
            o.set("tool_log",  ar.toolLog);
            o.set("rounds",    Jv::Num((double)ar.rounds));
            o.set("hit_limit", Jv::Bool(ar.hitLimit));
            o.set("fs_root",   Jv::Str(g_fsRootCanon.empty() ? g_fsRoot : g_fsRootCanon));
            o.set("fs_allow_outside", Jv::Bool(g_fsAllowOutside));
            o.set("shell",     Jv::Bool(ct.shell));
            if (!ar.error.empty()) o.set("error", Jv::Str(ar.error));
            sendResponse(cli, 200, "OK", "application/json; charset=utf-8", jsonDump(o));
            return;
        }

        // ---- 纯聊天（保持老行为） ----
        std::string j = callLLM(model, apiKey, baseUrl, messagesStr, temperature, timeout);
        sendResponse(cli, 200, "OK", "application/json; charset=utf-8", j);
        return;
    }
    
    sendResponse(cli, 404, "Not Found", "text/plain", "not found");
}

int main(int argc, char* argv[]){
    bool forkPerConn = true;
    for (int i = 1; i < argc; ++i){
        std::string a = argv[i];
        if      (a == "--sock" && i+1 < argc) g_sockPath = argv[++i];
        else if (a == "--port" && i+1 < argc) g_port     = std::atoi(argv[++i]);
        else if (a == "--bind" && i+1 < argc) g_bindIp   = argv[++i];
        else if (a == "--fs-root" && i+1 < argc) g_fsRoot = argv[++i];
        else if (a == "--fs-allow-outside")      g_fsAllowOutside = true;
        else if (a == "--max-tool-rounds" && i+1 < argc) g_maxToolRounds = std::atoi(argv[++i]);
        else if (a == "--chat-timeout"   && i+1 < argc) g_chatTimeout   = std::atoi(argv[++i]);
        else if (a == "--no-fork")               forkPerConn = false;
        else if (a == "-h" || a == "--help"){
            std::printf(
                "用法: llm_web [--port 8080] [--sock /tmp/llm_shell.sock] [--bind 127.0.0.1]\n"
                "              [--fs-root /] [--fs-allow-outside] [--max-tool-rounds 8]\n"
                "              [--chat-timeout 180] [--no-fork]\n"
                "\n"
                "  --fs-root PATH       文件工具/文件面板的工作区根目录（默认 /，即整机都能碰）\n"
                "  --fs-allow-outside   关掉越界检查：--fs-root 之外的路径也放行（默认禁止）；\n"
                "                       网页 Files 面板的勾选框也能随时收紧/放开，不用重启\n"
                "  --max-tool-rounds N  一次对话最多几轮工具调用（默认 8）\n"
                "  --chat-timeout N     Chat 一轮对话的时间预算秒数（默认 180，5~300）；\n"
                "                       网页上「超时(s)」输入框能改，这里是默认值与上限的出处\n"
                "  --no-fork            串行处理连接（默认每个连接一个子进程，chat 不会卡住终端页）\n");
            return 0;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    if (forkPerConn) signal(SIGCHLD, SIG_IGN);   // 父进程只管 accept，子进程自动回收

    // 算好沙箱根目录的规范路径（越界判断用）
    {
        std::error_code ec;
        fs::path r = fs::weakly_canonical(fs::path(g_fsRoot), ec);
        if (ec) r = fs::path(g_fsRoot);
        std::string s = r.string();
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        g_fsRoot      = s;
        g_fsRootCanon = s;
        std::error_code ec2;
        if (!fs::is_directory(s, ec2))
            std::fprintf(stderr, "[llm_web] 警告: 工作区 %s 不是已存在的目录，文件操作会全部失败\n", s.c_str());
    }
    // v1.3.5：把「启动参数定的范围」留一份，之后每个请求都从它重新开始
    g_fsRootBoot        = g_fsRoot;
    g_fsRootCanonBoot   = g_fsRootCanon;
    g_fsAllowOutsideBoot= g_fsAllowOutside;
    if (g_maxToolRounds < 1)  g_maxToolRounds = 1;
    if (g_maxToolRounds > 20) g_maxToolRounds = 20;
    if (g_chatTimeout < kChatTimeoutMin) g_chatTimeout = kChatTimeoutMin;
    if (g_chatTimeout > kChatTimeoutMax) g_chatTimeout = kChatTimeoutMax;

    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0){ std::perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_port);
    if (inet_pton(AF_INET, g_bindIp.c_str(), &addr.sin_addr) != 1){
        std::fprintf(stderr, "无效 --bind: %s\n", g_bindIp.c_str()); return 1;
    }
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0){ std::perror("bind"); return 1; }
    if (listen(srv, 16) < 0){ std::perror("listen"); return 1; }

    std::printf("[llm_web] 打开: http://%s:%d/    (daemon sock=%s)\n",
                g_bindIp.c_str(), g_port, g_sockPath.c_str());
    std::printf("[llm_web] 文件工作区: %s\n", g_fsRootCanon.c_str());
    if (g_fsAllowOutside)
        std::printf("[llm_web] 沙箱: 已放开越界限制（--fs-allow-outside）！任何路径都能操作\n");
    else
        std::printf("[llm_web] 沙箱: 只允许操作 %s 内的路径（要放开就加 --fs-allow-outside，\n"
                    "                       或用网页 Files 面板的勾选框随时改）\n",
                    g_fsRootCanon.c_str());
    std::printf("[llm_web] 提示: 只监听本机，请勿改 --bind 到公网地址。\n");
    std::fflush(stdout);

    while (true){
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0){ if (errno == EINTR) continue; std::perror("accept"); break; }
        if (!forkPerConn){
            handleHttp(cli);
            ::close(cli);
            continue;
        }
        pid_t pid = fork();
        if (pid < 0){                       // fork 失败就退化成串行
            handleHttp(cli);
            ::close(cli);
            continue;
        }
        if (pid == 0){
            ::close(srv);
            signal(SIGCHLD, SIG_DFL);       // 这个进程里还要 waitpid 收 curl，不能是 SIG_IGN
            handleHttp(cli);
            ::close(cli);
            _exit(0);
        }
        ::close(cli);                       // 父进程不等，SIG_IGN 会自动回收
    }
    ::close(srv);
    return 0;
}
