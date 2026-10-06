## 编译

std::filesystem 需要 C++17：

cd /data/hello
g++ -std=c++17 fs_demo.cpp -o fs_demo


> 如果 g++ 版本较老（GCC 8 及以前），需要额外链接：
>
> g++ -std=c++17 fs_demo.cpp -o fs_demo -lstdc++fs
> 


## 运行

列出当前目录：

./fs_demo


列出指定目录，例如根目录或用户目录：

./fs_demo /
./fs_demo /home/$USER
./fs_demo /data/hello


## 常用 std::filesystem 操作速查

| 功能 | 代码 |
|---|---|
| 判断是否存在 | fs::exists(path) |
| 是否为目录 | fs::is_directory(path) |
| 是否为普通文件 | fs::is_regular_file(path) |
| 文件大小 | fs::file_size(path) |
| 当前目录 | fs::current_path() |
| 遍历目录 | for (auto& e : fs::directory_iterator(path)) |
| 递归遍历 | fs::recursive_directory_iterator(path) |
| 复制 | fs::copy(src, dst) |
| 重命名 / 移动 | fs::rename(src, dst) |
| 删除 | fs::remove(path) |
| 递归删除 | fs::remove_all(path) |
| 创建目录 | fs::create_directory(path) |
| 创建多级目录 | fs::create_directories(path) |
| 创建符号链接 | fs::create_symlink(target, link) |

## 注意事项

1. 权限问题：访问 /、/etc 等系统目录时，普通用户可能读不到某些子项，程序会抛异常或跳过。可以在遍历时先用 fs::directory_options::skip_permission_denied 跳过无权限项：
   for (auto& e : fs::directory_iterator(path, fs::directory_options::skip_permission_denied))
   

2. 异常处理：std::filesystem 的函数默认抛 std::filesystem::filesystem_error，需要时可以 try/catch：
   try {
       fs::create_directories("/data/hello/subdir");
   } catch (const fs::filesystem_error& e) {
       std::cerr << "错误: " << e.what() << "\n";
   }
   

3. 平台差异：std::filesystem 在 Linux/macOS/Windows 下都能用，路径分隔符会自动适配。

如果你告诉我你想具体做哪件事（比如“递归扫描某个目录、按扩展名分类输出”或“读取所有文本文件内容”），我可以把程序改得更贴合你的需求。

---

## 在 Web 里用这些能力（v1.2.0 起已桥接，v1.2.1 加固）

fs_demo 的 list / read / write / gen / scan 已经接进 /data/hello/os/shell 的 web 服务：

- 网页上的 **Files 面板** 可以直接浏览 / 新建 / 编辑 / 删除文件，编码(UTF-8/GBK)和换行(CRLF/LF)都能选；
- 网页的 **Chat** 里勾上「文件操作」，模型就能真的建文件、改文件（function calling）；
- v1.2.1 起服务端会归一化各种网关返回的工具调用形态
  （arguments 给成对象、老式 function_call、content 是分片数组），
  所以模型在 Chat 里要建文件，换哪个网关都能真正落盘；
- 命令行等价于 POST /api/fs：

```bash
curl -sS -X POST http://127.0.0.1:8093/api/fs -H 'Content-Type: application/json' \
     -d '{"action":"list","path":"/data/hello/os/file"}'

curl -sS -X POST http://127.0.0.1:8093/api/fs -H 'Content-Type: application/json' \
     -d '{"action":"write","path":"/data/hello/tmp/demo.txt","content":"hello\n","eol":"crlf"}'
```

| fs_demo 命令 | /api/fs action | Chat 工具名 |
|---|---|---|
| fs_demo list <dir>            | list          | fs_list |
| fs_demo read <file>           | read          | fs_read |
| fs_demo write <file> <内容>    | write         | fs_write |
| （无）                        | append        | fs_append |
| fs_demo scan <dir> --ext ...  | scan          | fs_scan |
| fs_demo gen <tpl> <out> K=V   | gen           | fs_gen |
| （无）                        | mkdir / remove / move / copy / stat | fs_mkdir / fs_remove / fs_move / fs_copy |

所有操作都被限制在 web 服务的 --fs-root（默认 /data/hello）以内，越界会被拒绝。
详细说明见 ../shell/readme_llm_web.md。
