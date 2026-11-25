// src/trace.hpp — 轨迹 v1 的公开接口。
// 头文件只声明“数据长什么样”和“读写入口是什么”；公开接口不暴露缓冲区等实现细节。
// 为了保持实现最简，写入器在私有成员里直接持有 std::FILE*（因此需要包含 <cstdio>），
// 调用方只使用 write()/finish()，不接触这个句柄。
#pragma once  // 防止同一个编译单元重复包含本头文件，避免类重复定义。

#include <cstddef>  // 引入 std::size_t，用作记录下标类型。
#include <cstdint>  // 引入 std::uint32_t/std::uint64_t，保证字段宽度跨平台一致。
#include <cstdio>   // 引入 std::FILE，声明写入器持有的文件句柄类型。
#include <string>   // 引入 std::string，用于接收文件路径。
#include <vector>   // 引入 std::vector，读取器用它保存校验后的全部记录。

namespace memwhy {  // 项目命名空间，避免与第三方库的符号重名。

// TraceRecord 表示轨迹 v1 的一条记录，字段顺序与二进制契约逐字对应。
struct TraceRecord {
    std::uint32_t core = 0;      // 发起访问的核心编号；0 表示默认核心。
    std::uint32_t kind = 0;      // 访问类型：0=取指，1=读，2=写；其它值在读取时判为损坏。
    std::uint64_t addr = 0;      // 访问地址（字节地址）；64 位足以覆盖 SoC 物理地址空间。
    std::uint32_t width = 0;     // 访问宽度（字节数），必须大于 0。
    std::uint32_t reserved = 0;  // 保留字段，v1 必须写 0，为未来版本扩展留位置。
};

// TraceWriter 负责把记录按 v1 布局写入文件：先写占位头，逐条追加，最后回填记录数。
class TraceWriter {
public:
    explicit TraceWriter(const std::string& path);  // 打开文件并写入 16 字节占位头；失败时抛 std::runtime_error。
    ~TraceWriter();                                  // 析构时关闭仍然打开的文件句柄，避免资源泄漏。
    TraceWriter(const TraceWriter&) = delete;         // 禁止拷贝：文件句柄是独占资源，拷贝会导致双重关闭。
    TraceWriter& operator=(const TraceWriter&) = delete;  // 禁止拷贝赋值，理由与拷贝构造相同。
    void write(const TraceRecord& record);           // 追加一条 24 字节记录，内部显式小端序列化。
    void finish();                                   // 回填真实记录数并关闭文件；重复调用是幂等的。

private:
    std::FILE* file_ = nullptr;  // C 标准库文件句柄；nullptr 表示尚未打开或已经关闭。
    std::uint64_t count_ = 0;    // 已写出的记录条数，finish() 时写回文件头。
    bool finished_ = false;      // 是否已经收尾，防止回填后又被追加记录。
};

// TraceReader 负责读取轨迹：构造时一次性完成全部校验，next() 只做顺序遍历。
// ponytail: 以内存换实现简单；轨迹达到 GB 级时再改成流式两遍校验。
class TraceReader {
public:
    explicit TraceReader(const std::string& path);  // 打开并校验整个文件；任何损坏都抛 std::runtime_error 且说明原因。
    bool next(TraceRecord& record);                 // 取下一条记录；读完后返回 false，且不修改 record。
    std::size_t record_count() const;  // 返回校验通过后的记录总数；Bundle 用它推导默认测量窗口。

private:
    std::vector<TraceRecord> records_;  // 校验通过后的全部记录，按文件顺序保存。
    std::size_t index_ = 0;             // 下一条要返回的记录下标。
};

}  // namespace memwhy
