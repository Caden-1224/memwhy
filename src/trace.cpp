// src/trace.cpp — 轨迹 v1 二进制格式的读写实现。
// 本文件把内存中的 TraceRecord 按固定 24 字节布局写成小端字节流，
// 并在读回时按契约逐项校验。所有整数都显式移位序列化，绝不直接 fwrite 结构体，
// 因为结构体 padding 和主机字节序会让文件在不同平台上不一致。
#include "trace.hpp"  // 引入本模块的公开声明，保证定义与声明完全一致。

#include <cerrno>     // 提供 errno，fopen/fclose 失败时读取系统错误码。
#include <cstddef>    // 提供 std::size_t，用于文件大小与循环下标。
#include <cstdio>     // 提供 std::fopen/fread/fwrite/fseek/std::FILE 等 C 文件 API。
#include <cstring>    // 提供 std::memcmp/std::strerror，前者比较 magic，后者翻译 errno。
#include <limits>     // 提供 std::numeric_limits，计算记录数上限防止乘法溢出。
#include <stdexcept>  // 提供 std::runtime_error，作为统一抛出的异常类型。
#include <string>     // 提供 std::string/std::to_string，拼装错误信息。
#include <vector>     // 提供 std::vector，读取整个文件时按字节保存。

namespace memwhy {  // 与头文件保持同一个命名空间。

namespace {  // 匿名命名空间：下面的常量与辅助函数只在 trace.cpp 内部可见。

constexpr std::uint32_t kVersion = 1;  // 轨迹 v1 的版本号；文件头 offset 4 必须写 1。
constexpr std::uint64_t kHeaderSize = 16;  // 文件头固定 16 字节：4 字节 magic + 4 字节版本 + 8 字节记录数。
constexpr std::uint64_t kRecordSize = 24;  // 每条记录固定 24 字节：4+4+8+4+4。
constexpr unsigned char kMagic[4] = {'M', 'W', 'T', '1'};  // 文件标识 "MWT1"，用于快速识别不是轨迹文件。
constexpr std::uint64_t kMaxRecordCount = (std::numeric_limits<std::uint64_t>::max() - kHeaderSize) / kRecordSize;  // 记录数上限，保证 16 + 24×N 不溢出 64 位。

[[noreturn]] void fail(const std::string& message) {  // 统一抛错入口；[[noreturn]] 告诉编译器此函数不会正常返回。
    throw std::runtime_error(message);  // 用标准异常携带具体原因，调用方负责打印并退出。
}

void write_all(std::FILE* file, const unsigned char* data, std::size_t size) {  // 把一段字节完整写入文件。
    if (std::fwrite(data, 1, size, file) != size) {  // fwrite 返回实际写出的元素数；不相等说明磁盘满或 IO 出错。
        fail("trace: write failed");  // 写失败必须报错，避免静默产生半截文件。
    }
}

void put_u32(std::FILE* file, std::uint32_t value) {  // 以显式小端写入 32 位无符号整数。
    unsigned char bytes[4];  // 临时缓冲区，正好 4 字节。
    bytes[0] = static_cast<unsigned char>(value & 0xffu);  // 最低 8 位放在第 1 个字节（小端）。
    bytes[1] = static_cast<unsigned char>((value >> 8) & 0xffu);  // 次低 8 位放在第 2 个字节。
    bytes[2] = static_cast<unsigned char>((value >> 16) & 0xffu);  // 再高 8 位放在第 3 个字节。
    bytes[3] = static_cast<unsigned char>((value >> 24) & 0xffu);  // 最高 8 位放在第 4 个字节。
    write_all(file, bytes, 4);  // 把这 4 个字节写入文件。
}

void put_u64(std::FILE* file, std::uint64_t value) {  // 以显式小端写入 64 位无符号整数。
    unsigned char bytes[8];  // 临时缓冲区，正好 8 字节。
    for (int i = 0; i < 8; ++i) {  // 依次处理 8 个字节。
        bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xffu);  // 第 i 个字节取从低位数第 i×8 位开始的 8 位。
    }
    write_all(file, bytes, 8);  // 把这 8 个字节写入文件。
}

std::uint32_t get_u32(const unsigned char* bytes) {  // 从 4 个字节按小端还原 32 位整数。
    return static_cast<std::uint32_t>(bytes[0]) |  // 第 1 个字节是最低 8 位。
           (static_cast<std::uint32_t>(bytes[1]) << 8) |  // 第 2 个字节左移 8 位。
           (static_cast<std::uint32_t>(bytes[2]) << 16) |  // 第 3 个字节左移 16 位。
           (static_cast<std::uint32_t>(bytes[3]) << 24);  // 第 4 个字节左移 24 位。
}

std::uint64_t get_u64(const unsigned char* bytes) {  // 从 8 个字节按小端还原 64 位整数。
    std::uint64_t value = 0;  // 累加结果从 0 开始。
    for (int i = 0; i < 8; ++i) {  // 逐字节拼接。
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);  // 第 i 个字节左移 i×8 位后按位或进结果。
    }
    return value;  // 返回还原出的 64 位整数。
}

std::vector<unsigned char> read_whole_file(const std::string& path) {  // 把整个文件读成字节数组，便于一次性校验。
    std::FILE* file = std::fopen(path.c_str(), "rb");  // 以只读二进制模式打开；"rb" 防止换行转换。
    if (file == nullptr) {  // fopen 返回空指针表示打开失败。
        fail("trace: cannot open '" + path + "': " + std::strerror(errno));  // 把路径与系统错误原因一起报告。
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {  // 跳到文件末尾，准备测量文件长度。
        std::fclose(file);  // 先关闭句柄，避免异常路径泄漏。
        fail("trace: cannot seek to end of '" + path + "'");  // 报出具体的失败操作。
    }
    const long end = std::ftell(file);  // ftell 返回文件末尾偏移，即文件字节数。
    if (end < 0) {  // 负数表示 ftell 失败。
        std::fclose(file);  // 关闭句柄后再抛错。
        fail("trace: cannot tell size of '" + path + "'");  // 说明无法获取文件大小。
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {  // 把读指针移回文件开头。
        std::fclose(file);  // 关闭句柄避免泄漏。
        fail("trace: cannot rewind '" + path + "'");  // 说明回绕失败。
    }
    std::vector<unsigned char> data(static_cast<std::size_t>(end));  // 分配正好文件大小的缓冲区。
    if (!data.empty()) {  // 空文件不需要调用 fread。
        const std::size_t read = std::fread(data.data(), 1, data.size(), file);  // 一次读满缓冲区。
        if (read != data.size()) {  // 读到的字节数不足说明文件在读取过程中被截断或 IO 出错。
            std::fclose(file);  // 关闭句柄。
            fail("trace: short read on '" + path + "'");  // 报告短读错误。
        }
    }
    if (std::fclose(file) != 0) {  // 关闭文件；fclose 失败通常表示缓冲数据未能全部落盘。
        fail("trace: close failed for '" + path + "'");  // 关闭失败必须让调用方知道写入可能不完整。
    }
    return data;  // 返回完整的文件字节。
}

}  // 匿名命名空间结束。

TraceWriter::TraceWriter(const std::string& path) {  // 构造：打开文件并写入 16 字节占位头。
    std::FILE* file = std::fopen(path.c_str(), "wb");  // "wb" 创建或截断二进制文件，适合重新生成轨迹。
    if (file == nullptr) {  // 打开失败通常意味着目录不存在或没有写权限。
        fail("trace: cannot create '" + path + "': " + std::strerror(errno));  // 把路径与 errno 原因一起报告。
    }
    try {  // 头部写入可能失败，用 try 保证异常时关闭文件。
        write_all(file, kMagic, 4);  // 写 4 字节 magic "MWT1"。
        put_u32(file, kVersion);  // 写 4 字节版本号 1。
        put_u64(file, 0);  // 记录数先写 0 占位，finish() 时回填真实值。
    } catch (...) {  // 捕获任何写入失败。
        std::fclose(file);  // 关闭半成品文件。
        throw;  // 把原异常继续抛给调用方，不吞掉错误。
    }
    file_ = file;  // 只有头部完整写出后才保存句柄。
    count_ = 0;  // 内存计数从 0 开始。
    finished_ = false;  // 新写入器尚未收尾。
}

TraceWriter::~TraceWriter() {  // 析构：确保文件句柄不会泄漏。
    if (file_ != nullptr) {  // 只有仍然打开的文件才需要关闭。
        std::fclose(file_);  // 析构函数不抛异常；未 finish() 的文件会因记录数不匹配被读取器拒绝。
        file_ = nullptr;  // 清空指针，避免悬空。
    }
}

void TraceWriter::write(const TraceRecord& record) {  // 追加一条记录。
    if (finished_) {  // 已收尾后不允许再写。
        fail("trace: write after finish");  // 明确报错，避免记录数回填后文件继续增长。
    }
    if (file_ == nullptr) {  // 句柄为空说明文件未打开或已关闭。
        fail("trace: writer is not open");  // 报出使用错误。
    }
    if (record.kind > 2) {  // 写侧提前拦截非法访问类型，避免产出自己都读不回的文件。
        fail("trace: record kind must be 0, 1 or 2");  // 报出合法取值范围。
    }
    if (record.width == 0) {  // 访问宽度为 0 的记录没有意义，也会被读取器拒绝。
        fail("trace: record width must be > 0");  // 报出零宽度错误。
    }
    if (record.reserved != 0) {  // v1 的保留字段必须为 0。
        fail("trace: record reserved must be 0");  // 提前拦截脏值，保证写出的文件永远合法。
    }
    put_u32(file_, record.core);  // 写 4 字节 core。
    put_u32(file_, record.kind);  // 写 4 字节 kind。
    put_u64(file_, record.addr);  // 写 8 字节 addr。
    put_u32(file_, record.width);  // 写 4 字节 width。
    put_u32(file_, record.reserved);  // 写 4 字节 reserved（上面已保证为 0）。
    ++count_;  // 成功写出一条后计数加一。
}

void TraceWriter::finish() {  // 回填记录数并关闭文件。
    if (finished_) {  // 幂等：已经收尾就直接返回。
        return;  // 重复调用不产生副作用。
    }
    if (file_ == nullptr) {  // 未打开的文件无法收尾。
        fail("trace: writer is not open");  // 报出使用错误。
    }
    if (std::fseek(file_, 8, SEEK_SET) != 0) {  // 跳到文件头 offset 8，即记录数字段。
        std::fclose(file_);  // 关闭句柄。
        file_ = nullptr;  // 清空指针，避免析构再次关闭。
        fail("trace: cannot seek to record count");  // 报告定位失败。
    }
    put_u64(file_, count_);  // 把真实记录数以小端 8 字节写回。
    std::FILE* file = file_;  // 先保存句柄到局部变量。
    file_ = nullptr;  // 提前清空成员，防止后续抛异常时析构重复关闭。
    if (std::fclose(file) != 0) {  // 关闭文件并检查是否成功。
        fail("trace: close failed");  // 关闭失败说明数据可能没有完整落盘。
    }
    finished_ = true;  // 标记收尾完成。
}

TraceReader::TraceReader(const std::string& path) {  // 构造：一次读入整个文件并完成全部校验。
    const std::vector<unsigned char> data = read_whole_file(path);  // 读出全部字节，后续校验都在内存完成。
    if (data.size() < 4) {  // 连 4 字节 magic 都不够。
        fail("trace: file too small for magic (" + std::to_string(data.size()) + " bytes)");  // 明确报告实际大小。
    }
    if (std::memcmp(data.data(), kMagic, 4) != 0) {  // 按契约顺序先比较开头 4 字节是否为 "MWT1"。
        fail("trace: bad magic (expected 'MWT1')");  // 不是轨迹文件就立即拒绝。
    }
    if (data.size() < kHeaderSize) {  // magic 正确后再检查完整 16 字节头部。
        fail("trace: file too small for header (" + std::to_string(data.size()) + " bytes)");  // 明确报告实际大小。
    }
    const std::uint32_t version = get_u32(data.data() + 4);  // 读取 offset 4 的版本号。
    if (version != kVersion) {  // 只支持 v1。
        fail("trace: unsupported version " + std::to_string(version) + " (expected 1)");  // 报出版本号便于排查。
    }
    const std::uint64_t count = get_u64(data.data() + 8);  // 读取 offset 8 的记录数。
    if (count > kMaxRecordCount) {  // 记录数大到会让 16 + 24×N 溢出 64 位。
        fail("trace: size mismatch: record count " + std::to_string(count) + " is too large for any file");  // 归入大小不匹配类错误，避免计算溢出。
    }
    const std::uint64_t expected_size = kHeaderSize + kRecordSize * count;  // 按契约计算文件应有大小。
    if (static_cast<std::uint64_t>(data.size()) != expected_size) {  // 实际大小必须严格相等，截断或多出字节都算损坏。
        fail("trace: size mismatch: header says " + std::to_string(count) + " records (" + std::to_string(expected_size) + " bytes), file has " + std::to_string(data.size()) + " bytes");  // 报告期望值与实际值。
    }
    records_.reserve(static_cast<std::size_t>(count));  // 预分配记录数组，避免逐条扩容。
    for (std::uint64_t i = 0; i < count; ++i) {  // 逐条解析并校验记录。
        const std::size_t offset = static_cast<std::size_t>(kHeaderSize + i * kRecordSize);  // 计算第 i 条记录在字节数组中的起始偏移。
        const unsigned char* base = data.data() + offset;  // 取得该记录的起始指针。
        TraceRecord record;  // 准备一条记录对象。
        record.core = get_u32(base);  // offset 0：读取 core。
        record.kind = get_u32(base + 4);  // offset 4：读取 kind。
        record.addr = get_u64(base + 8);  // offset 8：读取 addr。
        record.width = get_u32(base + 16);  // offset 16：读取 width。
        record.reserved = get_u32(base + 20);  // offset 20：读取 reserved。
        if (record.reserved != 0) {  // v1 的保留字段必须为 0。
            fail("trace: record " + std::to_string(i) + " has non-zero reserved field (" + std::to_string(record.reserved) + ")");  // 报告第几条记录的保留字段非法。
        }
        if (record.kind > 2) {  // 合法访问类型只有 0=取指、1=读、2=写。
            fail("trace: record " + std::to_string(i) + " has invalid kind " + std::to_string(record.kind) + " (expected 0, 1, 2)");  // 报告非法 kind。
        }
        if (record.width == 0) {  // 访问宽度必须大于 0。
            fail("trace: record " + std::to_string(i) + " has zero width");  // 报告零宽度记录。
        }
        records_.push_back(record);  // 校验通过后保存记录。
    }
}

std::size_t TraceReader::record_count() const {  // 返回记录总数。
    return records_.size();  // vector 长度就是文件头声明的记录数（构造时已严格校验）。
}

bool TraceReader::next(TraceRecord& record) {  // 顺序取下一条记录。
    if (index_ >= records_.size()) {  // 下标越界说明已经读完。
        return false;  // 返回 false 表示没有更多记录。
    }
    record = records_[index_];  // 把当前记录拷贝到调用方提供的变量。
    ++index_;  // 下标前进到下一条。
    return true;  // 返回 true 表示成功取到一条记录。
}

}  // namespace memwhy
