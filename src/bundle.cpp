// src/bundle.cpp — Bundle 打包、manifest 读写与完整性校验的实现。
// 设计要点：先在同父目录的临时目录里写完所有成员并算好哈希，最后用 rename(2) 原子换名；
// 任何一步失败都只留下“什么都没有”的目标状态，不会出现半个 Bundle。
#include "bundle.hpp"  // 引入本模块的公开声明，保证定义与头文件一致。
#include "sha256.hpp"  // 引入 SHA-256，用于计算每个成员文件的摘要。
#include "trace.hpp"   // 引入 TraceReader，用它在打包前校验轨迹并取得记录数。

#include <algorithm>   // 提供 std::find，检查目录里是否有 manifest 未列出的文件。
#include <cerrno>      // 提供 errno，读取 open/fsync/rename 失败的系统错误码。
#include <cstdlib>     // 提供 std::getenv，读取测试注入用的环境变量。
#include <cstring>     // 提供 std::strerror，把 errno 翻译成人类可读文本。
#include <filesystem>  // 提供目录创建、复制、删除与路径拼接。
#include <fstream>     // 提供 std::ifstream/std::ofstream，读写文本文件与复制成员。
#include <iterator>    // 提供 std::istreambuf_iterator，一次读完整份 manifest。
#include <limits>      // 提供 std::numeric_limits，检查整数解析溢出。
#include <stdexcept>   // 提供 std::runtime_error，作为统一错误类型。
#include <string>      // 提供 std::string，保存路径、JSON 文本与错误信息。
#include <utility>     // 提供 std::move，用于把临时目录路径移交 guard。
#include <vector>      // 提供 std::vector，保存成员列表。

#include <fcntl.h>     // 提供 open 的 O_RDONLY/O_DIRECTORY 标志。
#include <unistd.h>    // 提供 fsync/close/getpid 等 POSIX 系统调用。

namespace memwhy {  // 与头文件保持同一个命名空间。
namespace {  // 匿名命名空间：以下辅助函数与类型只在 bundle.cpp 内部可见。

constexpr const char* kTraceMember = "trace.bin";        // 轨迹成员固定文件名，契约要求。
constexpr const char* kConfigMember = "config.json";     // 配置成员固定文件名，契约要求。
constexpr const char* kEnvMember = "env.json";           // 可选环境快照成员名，契约要求。
constexpr const char* kManifestName = "manifest.json";   // manifest 自身文件名，不参与成员哈希。

[[noreturn]] void fail(const std::string& message) {  // 统一抛错入口；[[noreturn]] 说明该函数不会正常返回。
    throw std::runtime_error(message);  // 用标准异常携带具体原因，CLI 负责打印并返回非零退出码。
}

// read_text_file：把整个文本文件读成字符串；失败时抛出带路径的错误。
std::string read_text_file(const std::filesystem::path& path) {  // 参数是要读取的文件路径。
    std::ifstream input(path, std::ios::binary);  // 二进制模式打开，manifest 的换行符保持原样。
    if (!input) {  // 打开失败通常表示文件不存在或没有读权限。
        fail("bundle: cannot read '" + path.string() + "'");  // 报告具体路径，方便定位。
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());  // 用迭代器范围一次读完全部字节。
    if (input.bad()) {  // badbit 表示真实 IO 错误；eofbit 是正常读完，不算错误。
        fail("bundle: read failed for '" + path.string() + "'");  // 读取中途出错必须报错，不能返回半份文本。
    }
    return text;  // 返回完整文本内容。
}

// write_text_file：把字符串原样写到文件；用于生成 manifest.json。
void write_text_file(const std::filesystem::path& path, const std::string& text) {  // 参数是目标路径与文本内容。
    std::ofstream output(path, std::ios::binary | std::ios::trunc);  // 二进制模式创建或截断目标文件。
    if (!output) {  // 打开失败说明目录不存在或没有写权限。
        fail("bundle: cannot write '" + path.string() + "'");  // 报告具体路径。
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));  // 一次性写出全部字节。
    output.close();  // 显式关闭，随后检查流状态。
    if (!output) {  // close 后 failbit/badbit 表示数据没有完整落盘。
        fail("bundle: write failed for '" + path.string() + "'");  // 报告写入失败。
    }
}

// copy_member：把一个输入文件复制成 Bundle 成员；错误信息包含源与目标路径。
void copy_member(const std::filesystem::path& source, const std::filesystem::path& target) {  // 源文件与目标成员路径。
    std::error_code error;  // 用 error_code 版本避免异常类型不受控。
    if (!std::filesystem::is_regular_file(source, error)) {  // 输入必须是普通文件；目录或不存在都要拒绝。
        fail("bundle: input is not a regular file: '" + source.string() + "'");  // 点名非法输入路径。
    }
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, error);  // 只复制字节，不覆盖已存在目标。
    if (error) {  // 复制失败时 error_code 保存具体原因。
        fail("bundle: cannot copy '" + source.string() + "' to '" + target.string() + "': " + error.message());  // 报告源、目标与系统原因。
    }
}

// sync_file：对单个文件调用 fsync，确保内容真正落盘后再 rename。
void sync_file(const std::filesystem::path& path) {  // 参数是要同步的文件路径。
    const std::string native = path.string();  // 先转成 std::string，保证 c_str() 在 POSIX 调用中类型正确。
    const int fd = ::open(native.c_str(), O_RDONLY);  // 以只读方式打开即可 fsync；fsync 不要求写权限。
    if (fd < 0) {  // 打开失败说明文件不存在或权限不足。
        fail("bundle: cannot open '" + native + "' for fsync: " + std::strerror(errno));  // 报告路径与系统原因。
    }
    if (::fsync(fd) != 0) {  // fsync 返回非零表示内核拒绝或设备错误。
        const int saved_errno = errno;  // 先保存 errno，close 可能覆盖它。
        ::close(fd);  // 关闭描述符，避免泄漏。
        fail("bundle: fsync failed for '" + native + "': " + std::strerror(saved_errno));  // 报告具体文件与原因。
    }
    if (::close(fd) != 0) {  // close 失败虽然少见，但也意味着状态不可信。
        fail("bundle: close failed for '" + native + "'");  // 报告关闭失败。
    }
}

// sync_directory：对目录本身调用 fsync，保证 rename 之后的目录项持久化。
void sync_directory(const std::filesystem::path& path) {  // 参数是要同步的目录路径。
    const std::string native = path.string();  // 转成 std::string 供 POSIX 调用。
    const int fd = ::open(native.c_str(), O_RDONLY | O_DIRECTORY);  // O_DIRECTORY 确保打开的是目录而不是普通文件。
    if (fd < 0) {  // 打开失败说明目录不存在或权限不足。
        fail("bundle: cannot open directory '" + native + "' for fsync: " + std::strerror(errno));  // 报告目录与原因。
    }
    if (::fsync(fd) != 0) {  // 目录 fsync 失败同样意味着元数据可能丢失。
        const int saved_errno = errno;  // 保存 errno 供报错使用。
        ::close(fd);  // 关闭描述符。
        fail("bundle: fsync failed for directory '" + native + "': " + std::strerror(saved_errno));  // 报告失败目录。
    }
    if (::close(fd) != 0) {  // 关闭失败也要让调用方知道。
        fail("bundle: close failed for directory '" + native + "'");  // 报告关闭失败。
    }
}

// TempDirGuard 用 RAII 管理临时目录：构造后接管目录，析构时若未 release 就递归删除。
class TempDirGuard {
public:
    explicit TempDirGuard(std::filesystem::path path) : path_(std::move(path)) {}  // 保存临时目录路径。
    ~TempDirGuard() {  // 析构时清理未提交的临时目录。
        if (!released_) {  // 只有还没成功 rename 的情况才需要清理。
            std::error_code error;  // 删除失败不应在析构中抛异常。
            std::filesystem::remove_all(path_, error);  // 递归删除临时目录，忽略错误码。
        }
    }
    TempDirGuard(const TempDirGuard&) = delete;  // 禁止拷贝：目录所有权必须唯一。
    TempDirGuard& operator=(const TempDirGuard&) = delete;  // 禁止拷贝赋值，理由同上。
    const std::filesystem::path& path() const { return path_; }  // 只读访问临时目录路径。
    void release() { released_ = true; }  // rename 成功后调用，放弃清理责任。
private:
    std::filesystem::path path_;  // 临时目录路径。
    bool released_ = false;        // 是否已经成功提交到目标位置。
};

// hex_digit：把单个十六进制字符转成 0..15 的数值；非法字符直接报 manifest 错误。
int hex_digit(char c) {  // 输入是一个 ASCII 字符。
    if (c >= '0' && c <= '9') {  // 数字 0-9。
        return c - '0';  // 直接相减得到数值。
    }
    if (c >= 'a' && c <= 'f') {  // 小写 a-f。
        return c - 'a' + 10;  // 字母转成 10..15。
    }
    if (c >= 'A' && c <= 'F') {  // 大写 A-F（JSON 允许，但 manifest 校验只接受小写摘要）。
        return c - 'A' + 10;  // 字母转成 10..15。
    }
    fail("manifest: invalid hex digit in \\u escape");  // 非法字符说明 JSON 本身不合法。
}

// JsonCursor 是一个只针对 manifest 所需子集的递归下降 JSON 读取器。
// 它只接受 manifest 契约需要的 JSON 子集，未知字段直接视为损坏。
class JsonCursor {
public:
    explicit JsonCursor(const std::string& text) : text_(text), pos_(0) {}  // 保存待解析文本与当前偏移。

    void skip_ws() {  // 跳过 JSON 允许的空白字符。
        while (pos_ < text_.size()) {  // 只要还没到文本末尾。
            const char c = text_[pos_];  // 取出当前字符。
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {  // JSON 的四种空白。
                ++pos_;  // 跳过这个空白字符。
            } else {  // 遇到非空白就停止。
                break;  // 交给调用方处理当前字符。
            }
        }
    }

    bool eof() const { return pos_ >= text_.size(); }  // 判断是否已经读完整个文本。

    bool consume(char expected) {  // 如果下一个非空白字符等于 expected 就吃掉并返回 true。
        skip_ws();  // 先跳过空白，JSON 允许记号之间任意空白。
        if (!eof() && text_[pos_] == expected) {  // 匹配成功。
            ++pos_;  // 前进一个字符。
            return true;  // 告诉调用方匹配成功。
        }
        return false;  // 不匹配时保持位置不变。
    }

    void expect(char expected) {  // 强制要求下一个非空白字符等于 expected。
        skip_ws();  // 跳过空白。
        if (eof() || text_[pos_] != expected) {  // 没遇到期望字符。
            fail(std::string("manifest: expected '") + expected + "'");  // 报告期望的字符。
        }
        ++pos_;  // 吃掉期望字符。
    }

    std::string parse_string() {  // 解析一个 JSON 字符串，返回解码后的内容。
        skip_ws();  // 跳过前导空白。
        if (eof() || text_[pos_] != '"') {  // 字符串必须以双引号开头。
            fail("manifest: expected string");  // 报告类型错误。
        }
        ++pos_;  // 跳过开头双引号。
        std::string out;  // 保存解码结果。
        while (true) {  // 一直读到闭合双引号。
            if (eof()) {  // 文本结束仍未闭合。
                fail("manifest: unterminated string");  // 报告字符串未结束。
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);  // 取出当前字节并前进。
            if (c == '"') {  // 遇到闭合双引号。
                break;  // 字符串解析完成。
            }
            if (c == '\\') {  // 反斜杠开始转义序列。
                if (eof()) {  // 转义序列不完整。
                    fail("manifest: unterminated escape");  // 报告错误。
                }
                const char escape = text_[pos_++];  // 取出转义字符。
                switch (escape) {  // 按 JSON 标准处理常见转义。
                    case '"': out.push_back('"'); break;  // \" -> "
                    case '\\': out.push_back('\\'); break;  // \\ -> 反斜杠
                    case '/': out.push_back('/'); break;  // \/ -> /
                    case 'b': out.push_back('\b'); break;  // \b -> 退格
                    case 'f': out.push_back('\f'); break;  // \f -> 换页
                    case 'n': out.push_back('\n'); break;  // \n -> 换行
                    case 'r': out.push_back('\r'); break;  // \r -> 回车
                    case 't': out.push_back('\t'); break;  // \t -> 制表符
                    case 'u': {  // \uXXXX -> Unicode 码点
                        if (pos_ + 4 > text_.size()) {  // 必须还有 4 个十六进制字符。
                            fail("manifest: truncated \\u escape");  // 报告截断。
                        }
                        int code = 0;  // 码点值。
                        for (int i = 0; i < 4; ++i) {  // 依次读 4 个十六进制字符。
                            code = code * 16 + hex_digit(text_[pos_++]);  // 左移 4 位并加上新数字。
                        }
                        if (code >= 0xD800 && code <= 0xDFFF) {  // 拒绝孤立代理项，manifest 不需要。
                            fail("manifest: unsupported surrogate escape");  // 报告不支持。
                        }
                        if (code < 0x80) {  // 单字节 UTF-8。
                            out.push_back(static_cast<char>(code));  // 直接放一个字节。
                        } else if (code < 0x800) {  // 双字节 UTF-8。
                            out.push_back(static_cast<char>(0xC0 | (code >> 6)));  // 第一个字节。
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));  // 第二个字节。
                        } else {  // 三字节 UTF-8。
                            out.push_back(static_cast<char>(0xE0 | (code >> 12)));  // 第一个字节。
                            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));  // 第二个字节。
                            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));  // 第三个字节。
                        }
                        break;  // 结束 \u 分支。
                    }
                    default: fail("manifest: invalid string escape");  // 其它转义都非法。
                }
            } else {  // 普通字符。
                if (c < 0x20) {  // JSON 不允许未转义的控制字符。
                    fail("manifest: control character in string");  // 报告非法控制字符。
                }
                out.push_back(static_cast<char>(c));  // 原样保存这个字节。
            }
        }
        return out;  // 返回解码后的字符串。
    }

    std::uint64_t parse_u64() {  // 解析一个非负十进制整数。
        skip_ws();  // 跳过前导空白。
        if (eof()) {  // 文本提前结束。
            fail("manifest: expected integer");  // 报告缺少整数。
        }
        if (text_[pos_] == '-') {  // 负数不允许出现在 warmup 或窗口字段。
            fail("manifest: negative integer not allowed");  // 报告非法负号。
        }
        if (text_[pos_] < '0' || text_[pos_] > '9') {  // 必须以数字开头。
            fail("manifest: expected integer");  // 报告类型错误。
        }
        std::uint64_t value = 0;  // 累加结果。
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {  // 逐位读取。
            const std::uint64_t digit = static_cast<std::uint64_t>(text_[pos_] - '0');  // 当前数字。
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {  // 检查乘 10 加 digit 是否溢出。
                fail("manifest: integer too large");  // 超出 64 位范围直接拒绝。
            }
            value = value * 10 + digit;  // 累加当前数字。
            ++pos_;  // 前进到下一位。
        }
        return value;  // 返回解析结果。
    }

private:
    const std::string& text_;  // 待解析的完整 JSON 文本（引用，不拷贝）。
    std::size_t pos_;          // 当前解析位置。
};

// validate_member_name：拒绝空名、路径分隔符与 manifest 自身，防止路径逃逸。
void validate_member_name(const std::string& name) {  // 输入是 manifest 中声明的成员名。
    if (name.empty()) {  // 空文件名没有意义。
        fail("manifest: member name is empty");  // 报告空名。
    }
    if (name == "." || name == "..") {  // 当前目录与上级目录不能作为成员。
        fail("manifest: invalid member name '" + name + "'");  // 报告非法名。
    }
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {  // 禁止任何路径分隔符。
        fail("manifest: member name must not contain path separators: '" + name + "'");  // 报告路径逃逸风险。
    }
    if (name == kManifestName) {  // manifest 不能把自己列为成员，否则哈希自指。
        fail("manifest: manifest.json cannot be a member");  // 报告非法自指。
    }
}

// validate_sha256：要求恰好 64 个小写十六进制字符，与 sha256sum 的输出格式一致。
void validate_sha256(const std::string& member_name, const std::string& sha256) {  // 成员名用于报错定位。
    if (sha256.size() != 64) {  // SHA-256 的十六进制表示固定 64 字符。
        fail("manifest: sha256 for member '" + member_name + "' must be 64 hex characters");  // 报告长度错误。
    }
    for (const char c : sha256) {  // 逐字符检查字符集。
        const bool lower_digit = (c >= '0' && c <= '9');  // 数字 0-9 合法。
        const bool lower_hex = (c >= 'a' && c <= 'f');  // 小写 a-f 合法。
        if (!lower_digit && !lower_hex) {  // 大写或其它字符都不符合契约。
            fail("manifest: sha256 for member '" + member_name + "' must be lowercase hex");  // 报告非法字符。
        }
    }
}

// parse_members：解析 manifest 的 members 数组。
void parse_members(JsonCursor& cursor, BundleManifest& manifest) {  // cursor 是当前 JSON 读取位置。
    cursor.expect('[');  // members 必须是数组。
    if (cursor.consume(']')) {  // 允许空数组语法上成立，但随后会被必需成员检查拒绝。
        return;  // 空数组直接结束。
    }
    while (true) {  // 逐个成员对象解析。
        cursor.expect('{');  // 每个成员必须是对象。
        ManifestMember member;  // 当前成员条目。
        bool have_name = false;  // 是否出现 name 字段。
        bool have_sha256 = false;  // 是否出现 sha256 字段。
        while (true) {  // 解析成员对象内的键值对。
            const std::string key = cursor.parse_string();  // 字段名必须是字符串。
            cursor.expect(':');  // 字段名与值之间必须有冒号。
            if (key == "name") {  // name 字段。
                if (have_name) {  // 重复字段说明 manifest 结构异常。
                    fail("manifest: duplicate name field in member");  // 报告重复。
                }
                member.name = cursor.parse_string();  // 读取成员文件名。
                have_name = true;  // 记录已出现。
            } else if (key == "sha256") {  // sha256 字段。
                if (have_sha256) {  // 重复字段同样拒绝。
                    fail("manifest: duplicate sha256 field in member");  // 报告重复。
                }
                member.sha256 = cursor.parse_string();  // 读取摘要文本。
                have_sha256 = true;  // 记录已出现。
            } else {  // 未知字段。
                fail("manifest: unknown field '" + key + "'");  // v1 只接受契约字段，未知字段视为损坏。
            }
            if (!cursor.consume(',')) {  // 没有逗号说明成员对象结束。
                break;  // 退出键值对循环。
            }
        }
        cursor.expect('}');  // 成员对象必须有右花括号。
        if (!have_name || !have_sha256) {  // 两个字段缺一不可。
            fail("manifest: member entry missing name or sha256");  // 报告结构不完整。
        }
        validate_member_name(member.name);  // 校验文件名安全。
        validate_sha256(member.name, member.sha256);  // 校验摘要格式。
        for (const ManifestMember& existing : manifest.members) {  // 检查是否与已有成员重名。
            if (existing.name == member.name) {  // 同名成员会导致哈希语义不明确。
                fail("manifest: duplicate member '" + member.name + "'");  // 报告重复成员。
            }
        }
        manifest.members.push_back(member);  // 保存这个成员。
        if (!cursor.consume(',')) {  // 没有逗号说明数组结束。
            break;  // 退出成员循环。
        }
    }
    cursor.expect(']');  // 数组必须有右方括号。
}

// parse_measure_window：解析 measure_window 对象。
void parse_measure_window(JsonCursor& cursor, MeasureWindow& window) {  // cursor 是当前 JSON 读取位置。
    cursor.expect('{');  // 测量窗口必须是对象。
    bool have_start = false;  // 是否出现 start 字段。
    bool have_count = false;  // 是否出现 count 字段。
    if (!cursor.consume('}')) {  // 非空对象才需要逐字段解析。
        while (true) {  // 逐个键值对解析。
            const std::string key = cursor.parse_string();  // 字段名。
            cursor.expect(':');  // 冒号分隔符。
            if (key == "start") {  // start 字段。
                if (have_start) {  // 重复字段拒绝。
                    fail("manifest: duplicate start field in measure_window");  // 报告重复。
                }
                window.start = cursor.parse_u64();  // 读取起始下标。
                have_start = true;  // 记录已出现。
            } else if (key == "count") {  // count 字段。
                if (have_count) {  // 重复字段拒绝。
                    fail("manifest: duplicate count field in measure_window");  // 报告重复。
                }
                window.count = cursor.parse_u64();  // 读取窗口条数。
                have_count = true;  // 记录已出现。
            } else {  // 未知字段。
                fail("manifest: unknown field '" + key + "'");  // v1 只接受契约字段，未知字段视为损坏。
            }
            if (!cursor.consume(',')) {  // 没有逗号说明对象结束。
                break;  // 退出循环。
            }
        }
        cursor.expect('}');  // 对象必须有右花括号。
    }
    if (!have_start || !have_count) {  // 两个字段缺一不可。
        fail("manifest: measure_window must contain start and count");  // 报告结构不完整。
    }
}

// parse_manifest：解析整份 manifest.json，并做契约级校验。
BundleManifest parse_manifest(const std::string& text) {  // 输入是 manifest 的完整文本。
    JsonCursor cursor(text);  // 创建解析器。
    BundleManifest manifest;  // 解析结果。
    bool have_members = false;  // 是否出现 members 字段。
    bool have_warmup = false;  // 是否出现 warmup 字段。
    bool have_window = false;  // 是否出现 measure_window 字段。
    cursor.skip_ws();  // 跳过根对象前空白。
    cursor.expect('{');  // 根必须是对象。
    if (!cursor.consume('}')) {  // 空对象直接结束，随后字段检查会报错。
        while (true) {  // 逐个根字段解析。
            const std::string key = cursor.parse_string();  // 字段名。
            cursor.expect(':');  // 冒号分隔符。
            if (key == "members") {  // members 字段。
                if (have_members) {  // 重复字段拒绝。
                    fail("manifest: duplicate members field");  // 报告重复。
                }
                parse_members(cursor, manifest);  // 解析成员数组。
                have_members = true;  // 记录已出现。
            } else if (key == "warmup") {  // warmup 字段。
                if (have_warmup) {  // 重复字段拒绝。
                    fail("manifest: duplicate warmup field");  // 报告重复。
                }
                manifest.warmup = cursor.parse_u64();  // 读取预热记录数。
                have_warmup = true;  // 记录已出现。
            } else if (key == "measure_window") {  // measure_window 字段。
                if (have_window) {  // 重复字段拒绝。
                    fail("manifest: duplicate measure_window field");  // 报告重复。
                }
                parse_measure_window(cursor, manifest.measure_window);  // 解析测量窗口。
                have_window = true;  // 记录已出现。
            } else {  // 未知字段。
                fail("manifest: unknown field '" + key + "'");  // v1 只接受契约字段，未知字段视为损坏。
            }
            if (!cursor.consume(',')) {  // 没有逗号说明根对象结束。
                break;  // 退出循环。
            }
        }
        cursor.expect('}');  // 根对象必须有右花括号。
    }
    cursor.skip_ws();  // 跳过根对象后空白。
    if (!cursor.eof()) {  // JSON 之后不允许还有其它内容。
        fail("manifest: trailing data after JSON object");  // 报告多余数据。
    }
    if (!have_members || !have_warmup || !have_window) {  // 三个必需字段缺一不可。
        fail("manifest: missing required field (members, warmup, measure_window)");  // 报告缺失字段。
    }
    if (manifest.members.size() < 2 || manifest.members.size() > 3) {  // 成员数量必须是两个或三个（可选 env.json）。
        fail("manifest: members must be trace.bin, config.json and optional env.json");  // 报告成员集合非法。
    }
    if (manifest.members[0].name != kTraceMember) {  // 契约要求第一个成员固定为 trace.bin。
        fail("manifest: first member must be trace.bin");  // 报告顺序错误。
    }
    if (manifest.members[1].name != kConfigMember) {  // 契约要求第二个成员固定为 config.json。
        fail("manifest: second member must be config.json");  // 报告顺序错误。
    }
    if (manifest.members.size() == 3 && manifest.members[2].name != kEnvMember) {  // 第三个成员只能是可选 env.json。
        fail("manifest: third member must be env.json");  // 报告多余成员。
    }
    return manifest;  // 返回解析并校验后的 manifest。
}

// json_escape：把字符串转义成 JSON 字符串字面量内部形式。
std::string json_escape(const std::string& text) {  // 输入是成员名等短字符串。
    std::string out;  // 保存转义结果。
    for (const unsigned char c : text) {  // 逐字节处理。
        switch (c) {  // 按 JSON 规则转义特殊字符。
            case '"': out += "\\\""; break;  // 双引号需要反斜杠转义。
            case '\\': out += "\\\\"; break;  // 反斜杠自身需要双写。
            case '\n': out += "\\n"; break;  // 换行转义。
            case '\r': out += "\\r"; break;  // 回车转义。
            case '\t': out += "\\t"; break;  // 制表符转义。
            default:  // 其它字符。
                if (c < 0x20) {  // 控制字符必须转义，但成员名不会出现。
                    fail("bundle: unsupported control character in member name");  // 直接拒绝。
                }
                out.push_back(static_cast<char>(c));  // 普通字符原样输出。
        }
    }
    return out;  // 返回转义后的文本。
}

// make_manifest：按固定格式生成 manifest.json；成员顺序固定，保证同输入逐字节一致。
std::string make_manifest(const std::vector<ManifestMember>& members, std::uint64_t warmup, const MeasureWindow& window) {  // 输入是成员与窗口参数。
    std::string out;  // 输出文本。
    out += "{\n";  // 根对象开始。
    out += "  \"members\": [\n";  // members 数组开始。
    for (std::size_t i = 0; i < members.size(); ++i) {  // 按固定顺序输出每个成员。
        out += "    {\"name\": \"" + json_escape(members[i].name) + "\", \"sha256\": \"" + members[i].sha256 + "\"}";  // 输出成员对象。
        if (i + 1 < members.size()) {  // 不是最后一个成员时。
            out += ",";  // 补数组分隔逗号。
        }
        out += "\n";  // 每个成员一行，便于人工审阅。
    }
    out += "  ],\n";  // members 数组结束。
    out += "  \"warmup\": " + std::to_string(warmup) + ",\n";  // warmup 字段。
    out += "  \"measure_window\": {\"start\": " + std::to_string(window.start) + ", \"count\": " + std::to_string(window.count) + "}\n";  // 测量窗口字段。
    out += "}\n";  // 根对象结束。
    return out;  // 返回完整 manifest 文本。
}

}  // 匿名命名空间结束。

void pack_bundle(const BundlePackOptions& options) {  // 打包入口：完成校验、复制、哈希、写 manifest、fsync 与原子重命名。
    if (options.trace_path.empty() || options.config_path.empty() || options.out_dir.empty()) {  // 三个必填参数不能为空。
        fail("bundle: --trace, --config and --out are required");  // 报告缺参。
    }
    const TraceReader trace_reader(options.trace_path);  // 先完整校验轨迹；损坏输入在创建任何目录前就被拒绝。
    const std::uint64_t record_count = static_cast<std::uint64_t>(trace_reader.record_count());  // 取得轨迹记录总数。
    if (options.measure_start > record_count) {  // 测量窗口起点不能越过轨迹末尾。
        fail("bundle: measure start exceeds trace record count");  // 报告非法窗口。
    }
    const std::uint64_t available = record_count - options.measure_start;  // 从起点到末尾还有多少条记录。
    const std::uint64_t measure_count = options.measure_count_set ? options.measure_count : available;  // 未显式给出时默认覆盖剩余全部记录。
    if (measure_count > available) {  // 窗口长度不能超过剩余记录数。
        fail("bundle: measure window exceeds trace record count");  // 报告非法窗口。
    }
    if (options.warmup > record_count) {  // 预热记录数不能超过总记录数。
        fail("bundle: warmup exceeds trace record count");  // 报告非法预热。
    }
    std::string out_text = options.out_dir;  // 复制输出路径文本，便于去掉末尾分隔符。
    while (out_text.size() > 1 && (out_text.back() == '/' || out_text.back() == '\\')) {  // 去掉用户习惯性输入的末尾斜杠。
        out_text.pop_back();  // 删除最后一个分隔符。
    }
    const std::filesystem::path out(out_text);  // 规范化后的输出目录路径。
    if (out.filename().empty()) {  // 根目录或空文件名不能作为 Bundle 目标。
        fail("bundle: invalid output directory '" + options.out_dir + "'");  // 报告非法输出路径。
    }
    const std::filesystem::path parent = out.parent_path().empty() ? std::filesystem::path(".") : out.parent_path();  // 父目录用于放临时目录；空表示当前目录。
    std::error_code error;  // 复用错误码对象。
    if (!std::filesystem::is_directory(parent, error)) {  // 父目录必须存在且是目录。
        fail("bundle: parent directory does not exist: '" + parent.string() + "'");  // 报告父目录缺失。
    }
    if (std::filesystem::exists(out, error)) {  // 已存在的目标不能覆盖，避免误删用户数据。
        fail("bundle: output already exists: '" + out.string() + "'");  // 报告目标冲突。
    }
    const std::filesystem::path temp = parent / (out.filename().string() + ".tmp." + std::to_string(static_cast<long long>(::getpid())));  // 临时目录与目标同父目录，保证 rename 不跨文件系统。
    std::filesystem::remove_all(temp, error);  // 清掉上次异常退出可能留下的同名临时目录。
    error.clear();  // 清除 remove_all 可能写入的旧错误码。
    if (!std::filesystem::create_directories(temp, error)) {  // 创建本次的临时目录。
        fail("bundle: cannot create temporary directory '" + temp.string() + "': " + error.message());  // 报告创建失败原因。
    }
    TempDirGuard guard(temp);  // 从这一刻起，任何异常都会自动删除临时目录。
    copy_member(options.trace_path, temp / kTraceMember);  // 复制轨迹为 trace.bin。
    copy_member(options.config_path, temp / kConfigMember);  // 复制配置为 config.json。
    if (!options.env_path.empty()) {  // 只有显式传入 --env 才打包环境快照。
        copy_member(options.env_path, temp / kEnvMember);  // 复制环境快照为 env.json。
    }
    std::vector<ManifestMember> members;  // 按固定顺序收集成员。
    members.push_back({kTraceMember, sha256_file_hex((temp / kTraceMember).string())});  // 计算并记录 trace.bin 的摘要。
    members.push_back({kConfigMember, sha256_file_hex((temp / kConfigMember).string())});  // 计算并记录 config.json 的摘要。
    if (!options.env_path.empty()) {  // 有环境快照时。
        members.push_back({kEnvMember, sha256_file_hex((temp / kEnvMember).string())});  // 计算并记录 env.json 的摘要。
    }
    MeasureWindow window;  // 计算最终写入 manifest 的测量窗口。
    window.start = options.measure_start;  // 起始下标来自参数。
    window.count = measure_count;  // 条数来自参数或默认值。
    write_text_file(temp / kManifestName, make_manifest(members, options.warmup, window));  // 写出 manifest.json。
    (void)verify_bundle(temp.string());  // 写完后立即自检，确保 manifest 与成员一致。
    for (const ManifestMember& member : members) {  // 逐个成员文件调用 fsync。
        sync_file(temp / member.name);  // 保证成员内容落盘。
    }
    sync_file(temp / kManifestName);  // manifest 也要落盘。
    sync_directory(temp);  // 目录项落盘，随后才能安全 rename。
    const char* fail_hook = std::getenv("MEMWHY_TEST_PACK_FAIL_BEFORE_RENAME");  // 测试用注入点：在 rename 前强制失败。
    if (fail_hook != nullptr && std::strcmp(fail_hook, "1") == 0) {  // 只有测试进程显式设置时才触发。
        fail("bundle: injected failure before rename");  // 模拟中途失败，验证临时目录被清理。
    }
    const std::string temp_native = temp.string();  // 转成 std::string 供 rename 使用。
    const std::string out_native = out.string();  // 目标路径同样转成 std::string。
    if (::rename(temp_native.c_str(), out_native.c_str()) != 0) {  // rename(2) 在同一文件系统内原子完成目录换名。
        fail("bundle: cannot rename '" + temp_native + "' to '" + out_native + "': " + std::strerror(errno));  // 报告重命名失败。
    }
    guard.release();  // rename 成功，临时目录已变成目标目录，不再清理。
    sync_directory(parent);  // 父目录项也落盘，保证崩溃后目标目录仍然可见。
}

BundleManifest verify_bundle(const std::string& bundle_dir) {  // 校验入口：读 manifest、逐成员核对哈希。
    const std::filesystem::path dir(bundle_dir);  // 目标 Bundle 目录。
    std::error_code error;  // 复用错误码对象。
    if (!std::filesystem::is_directory(dir, error)) {  // Bundle 必须是目录。
        fail("bundle: not a directory: '" + bundle_dir + "'");  // 报告路径类型错误。
    }
    const std::filesystem::path manifest_path = dir / kManifestName;  // manifest 固定路径。
    if (!std::filesystem::is_regular_file(manifest_path, error)) {  // manifest 必须存在且是普通文件。
        fail("bundle: missing manifest.json in '" + bundle_dir + "'");  // 报告 manifest 缺失。
    }
    const BundleManifest manifest = parse_manifest(read_text_file(manifest_path));  // 读取并解析 manifest。
    std::vector<std::string> allowed_names;  // 目录允许出现的文件名集合。
    allowed_names.push_back(kManifestName);  // manifest 自身允许存在。
    for (const ManifestMember& member : manifest.members) {  // 把每个声明成员加入允许集合。
        allowed_names.push_back(member.name);  // 记录成员名。
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {  // 遍历 Bundle 目录。
        const std::string name = entry.path().filename().string();  // 当前条目文件名。
        if (!entry.is_regular_file()) {  // Bundle 里不允许子目录或特殊文件。
            fail("bundle: unexpected entry '" + name + "'");  // 报告意外条目。
        }
        if (std::find(allowed_names.begin(), allowed_names.end(), name) == allowed_names.end()) {  // 文件不在 manifest 与 manifest 自身之外。
            fail("bundle: unexpected file '" + name + "'");  // 报告多余文件，保证 Bundle 成员集合确定。
        }
    }
    for (const ManifestMember& member : manifest.members) {  // 逐个成员校验。
        const std::filesystem::path member_path = dir / member.name;  // 成员路径由目录与文件名拼接。
        if (!std::filesystem::is_regular_file(member_path, error)) {  // 成员必须存在且是普通文件。
            fail("bundle: missing member '" + member.name + "'");  // 点名缺失成员。
        }
        const std::string actual = sha256_file_hex(member_path.string());  // 重新计算成员摘要。
        if (actual != member.sha256) {  // 与 manifest 中记录的摘要比较。
            fail("bundle: checksum mismatch for member '" + member.name + "'");  // 点名被篡改或损坏的成员。
        }
    }
    return manifest;  // 返回 manifest，供调用方读取窗口参数。
}

}  // namespace memwhy