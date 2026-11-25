// tests/trace_tests.cpp — 轨迹 v1 格式的 CTest 自检程序。
// 本文件不用任何测试框架，靠“检查条件、累计失败数、用退出码汇报”的朴素方式，
// 验证 trace.hpp 的公开行为：二进制布局、读写往返、同种子复现、损坏文件报错。
#include "random.hpp"  // 引入固定种子采样工具，用于生成可复现的测试记录。
#include "trace.hpp"   // 引入被测试的轨迹读写接口。
#include <cstddef>     // 引入 std::size_t，表示记录条数与下标。
#include <cstdint>     // 引入 std::uint32_t/uint64_t，保证整数字段宽度固定。
#include <cstdio>      // 引入 std::remove 等 C 文件操作函数。
#include <cstring>     // 引入 std::memcmp，用于逐字节比较。
#include <filesystem>  // 引入目录创建/删除工具，管理测试临时目录。
#include <fstream>     // 引入 std::ifstream/std::ofstream，读写整文件。
#include <iostream>    // 引入 std::cout/std::cerr，打印测试结果。
#include <iterator>    // 引入 std::istreambuf_iterator，一次读入全部字节。
#include <random>      // 引入 std::mt19937_64，生成固定种子的随机序列。
#include <stdexcept>   // 引入 std::runtime_error，捕获预期中的读取错误。
#include <string>      // 引入 std::string，保存路径与错误信息。
#include <vector>      // 引入 std::vector，保存记录列表与字节序列。

namespace {  // 匿名命名空间让所有辅助函数只在本文件可见，避免链接时符号冲突。

int failures = 0;  // 失败计数器：每失败一个检查就加一，最后决定进程退出码。

// check：断言一个条件成立；失败时打印原因并累计失败次数。
void check(bool condition, const std::string& what) {
    if (!condition) {  // 条件为假，说明这条测试预期没有满足。
        std::cerr << "FAIL: " << what << '\n';  // 把失败原因写到标准错误，CTest 日志可见。
        ++failures;  // 累计失败数，让 main 最终返回 1。
    }
}

// read_bytes：把文件完整读成字节数组，用于逐字节比较头部或两份文件。
std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);  // 以二进制模式打开，避免换行符被转换。
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());  // 用迭代器范围构造 vector，一次读完。
}

// write_bytes：把字节数组原样写回文件，用来制造各种损坏样例。
void write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary);  // 以二进制模式创建或覆盖目标文件。
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));  // 把缓冲区里的每个字节原样写出。
}

// generate_records：用固定种子生成一批可复现的测试记录。
std::vector<memwhy::TraceRecord> generate_records(std::uint64_t seed, std::size_t count) {
    std::mt19937_64 rng(seed);  // 用种子初始化梅森旋转 64 位随机数引擎，同种子必然同序列。
    std::vector<memwhy::TraceRecord> records;  // 准备一个容器保存生成的记录。
    records.reserve(count);  // 预先分配容量，避免循环中反复扩容。
    for (std::size_t i = 0; i < count; ++i) {  // 逐条生成记录。
        memwhy::TraceRecord record;  // 默认初始化一条记录（字段全为 0）。
        record.core = static_cast<std::uint32_t>(memwhy::uniform_below(rng, 4));  // 核心号取 [0,4) 的均匀整数。
        record.kind = static_cast<std::uint32_t>(memwhy::uniform_below(rng, 3));  // 访问类型取 [0,3)，覆盖取指/读/写。
        record.addr = memwhy::uniform_below(rng, 1ULL << 32);  // 地址取 32 位范围内的均匀值，便于人工核对。
        record.width = static_cast<std::uint32_t>(1 + memwhy::uniform_below(rng, 64));  // 宽度取 [1,64]，保证大于 0。
        record.reserved = 0;  // v1 的保留字段必须写 0。
        records.push_back(record);  // 把这条记录追加到列表。
    }
    return records;  // 返回生成好的记录列表。
}

// write_trace：用 TraceWriter 把记录列表写成一个轨迹文件。
void write_trace(const std::filesystem::path& path, const std::vector<memwhy::TraceRecord>& records) {
    memwhy::TraceWriter writer(path.string());  // 创建写入器；构造时写入 16 字节占位头。
    for (const memwhy::TraceRecord& record : records) {  // 按顺序遍历每条记录。
        writer.write(record);  // 追加一条 24 字节记录。
    }
    writer.finish();  // 回填真实记录数并关闭文件。
}

// make_two_record_fixture：构造微例与损坏样例共用的两条记录（8 字节读 @0、8 字节写 @64）。
std::vector<memwhy::TraceRecord> make_two_record_fixture() {
    std::vector<memwhy::TraceRecord> records;  // 创建记录列表。
    memwhy::TraceRecord first;  // 创建第 1 条记录。
    first.core = 0;  // 第 1 条记录的核心号为 0。
    first.kind = 1;  // kind=1 表示读访问。
    first.addr = 0;  // 地址为 0。
    first.width = 8;  // 宽度 8 字节。
    first.reserved = 0;  // 保留字段为 0。
    memwhy::TraceRecord second;  // 创建第 2 条记录。
    second.core = 0;  // 第 2 条记录的核心号为 0。
    second.kind = 2;  // kind=2 表示写访问。
    second.addr = 64;  // 地址为 64。
    second.width = 8;  // 宽度 8 字节。
    second.reserved = 0;  // 保留字段为 0。
    records.push_back(first);  // 把第 1 条记录加入列表。
    records.push_back(second);  // 把第 2 条记录加入列表。
    return records;  // 返回这两条记录，供多个测试复用。
}

// test_micro_example：验证工单给出的两记录微例，逐字节检查头部。
void test_micro_example(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "micro.bin";  // 微例输出文件路径。
    const std::vector<memwhy::TraceRecord> records = make_two_record_fixture();  // 复用公共的两记录夹具。
    write_trace(path, records);  // 写出微例文件。
    const std::vector<unsigned char> bytes = read_bytes(path);  // 读回文件字节用于断言。
    check(bytes.size() == 64, "微例文件大小应为 16 + 24×2 = 64 字节");  // 文件大小必须等于 64。
    const unsigned char expected_header[16] = {  // 工单规定的头部 16 字节。
        0x4d, 0x57, 0x54, 0x31,  // "MWT1" 的 ASCII 字节。
        0x01, 0x00, 0x00, 0x00,  // 版本号 1 的小端 4 字节。
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 记录数 2 的小端 8 字节。
    };
    check(bytes.size() >= 16 && std::memcmp(bytes.data(), expected_header, 16) == 0, "微例头部 16 字节应为 4d 57 54 31 01 00 00 00 02 00 00 00 00 00 00 00");  // 头部逐字节比较。
    memwhy::TraceReader reader(path.string());  // 用读取器打开并校验文件。
    memwhy::TraceRecord out;  // 准备接收读出的记录。
    check(reader.next(out), "微例应能读出第 1 条记录");  // 第 1 条必须存在。
    check(out.core == 0 && out.kind == 1 && out.addr == 0 && out.width == 8 && out.reserved == 0, "微例第 1 条记录字段应完整还原");  // 逐字段核对第 1 条。
    check(reader.next(out), "微例应能读出第 2 条记录");  // 第 2 条必须存在。
    check(out.core == 0 && out.kind == 2 && out.addr == 64 && out.width == 8 && out.reserved == 0, "微例第 2 条记录字段应完整还原");  // 逐字段核对第 2 条。
    check(!reader.next(out), "微例只有 2 条记录，第 3 次 next 应返回 false");  // 读完后必须返回 false。
}

// test_roundtrip：随机生成一批记录，写入后逐字段读回比较。
void test_roundtrip(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "roundtrip.bin";  // 往返测试文件路径。
    const std::vector<memwhy::TraceRecord> records = generate_records(0x123456789abcdefULL, 257);  // 用固定种子生成 257 条（奇数便于发现尾条丢失）。
    write_trace(path, records);  // 写入轨迹文件。
    memwhy::TraceReader reader(path.string());  // 创建读取器，构造时完成全部校验。
    memwhy::TraceRecord out;  // 准备接收读出的记录。
    std::size_t index = 0;  // 记录已读条数。
    while (reader.next(out)) {  // 一直读到 next 返回 false 为止。
        check(index < records.size(), "读回的记录数不应超过写入数");  // 防御性检查，避免越界访问。
        if (index < records.size()) {  // 只有下标合法时才逐字段比较。
            const memwhy::TraceRecord& expected = records[index];  // 取出对应的原始记录。
            check(out.core == expected.core && out.kind == expected.kind && out.addr == expected.addr && out.width == expected.width && out.reserved == expected.reserved, "第 " + std::to_string(index) + " 条记录字段应与写入一致");  // 五个字段全部相等才算往返无损。
        }
        ++index;  // 下标前进到下一条。
    }
    check(index == records.size(), "读回记录总数应等于写入记录数");  // 数量必须完全一致。
}

// test_u64_address_roundtrip：覆盖 addr 的高 32 位，防止只测低半部漏掉序列化错误。
void test_u64_address_roundtrip(const std::filesystem::path& dir) {
    const std::filesystem::path path = dir / "u64_addr.bin";  // 测试文件路径。
    const std::uint64_t addresses[] = {0x00000000ffffffffULL, 0x123456789abcdef0ULL, 0x8000000000000000ULL, 0xffffffffffffffffULL};  // 覆盖低半部全 1、高低混合、最高位与全 1 四种边界。
    std::vector<memwhy::TraceRecord> records;  // 记录列表。
    for (std::size_t i = 0; i < 4; ++i) {  // 依次为每个地址生成一条记录。
        memwhy::TraceRecord record;  // 默认初始化一条记录。
        record.core = 1;  // 核心号 1。
        record.kind = 1;  // 读访问。
        record.addr = addresses[i];  // 使用当前边界地址。
        record.width = 8;  // 宽度 8 字节。
        records.push_back(record);  // 加入记录列表。
    }
    write_trace(path, records);  // 写出文件。
    memwhy::TraceReader reader(path.string());  // 读回文件，构造时完成校验。
    memwhy::TraceRecord out;  // 准备接收读出的记录。
    for (std::size_t i = 0; i < 4; ++i) {  // 逐条核对地址。
        check(reader.next(out), "u64 地址用例应能读出记录");  // 每条都必须存在。
        check(out.addr == addresses[i], "高 32 位地址应完整往返");  // 地址必须逐位一致。
    }
    check(!reader.next(out), "u64 地址用例记录数应为 4");  // 不应多出记录。
}

// test_same_seed_byte_identical：同一种子生成两次，文件必须逐字节一致。
void test_same_seed_byte_identical(const std::filesystem::path& dir) {
    const std::filesystem::path first_path = dir / "seed_a.bin";  // 第一次生成的文件路径。
    const std::filesystem::path second_path = dir / "seed_b.bin";  // 第二次生成的文件路径。
    const std::vector<memwhy::TraceRecord> first_records = generate_records(42, 1000);  // 第一次：种子 42，1000 条。
    const std::vector<memwhy::TraceRecord> second_records = generate_records(42, 1000);  // 第二次：同样的种子与条数。
    write_trace(first_path, first_records);  // 写出第一份文件。
    write_trace(second_path, second_records);  // 写出第二份文件。
    const std::vector<unsigned char> first_bytes = read_bytes(first_path);  // 读回第一份字节。
    const std::vector<unsigned char> second_bytes = read_bytes(second_path);  // 读回第二份字节。
    check(first_bytes == second_bytes, "同种子两次生成的轨迹文件必须逐字节一致");  // vector 逐元素比较等价于逐字节比较。
}

// expect_reader_error：断言某份损坏字节会让 TraceReader 抛错，且错误信息包含指定关键词。
void expect_reader_error(const std::filesystem::path& dir, const std::string& name, const std::vector<unsigned char>& bytes, const std::string& expected_substring) {
    const std::filesystem::path path = dir / name;  // 损坏样例的文件路径。
    write_bytes(path, bytes);  // 把损坏字节写成真实文件。
    try {  // 读取器预期在构造时抛出 std::runtime_error。
        memwhy::TraceReader reader(path.string());  // 尝试打开损坏文件。
        (void)reader;  // 显式忽略变量，避免“未使用”警告。
        check(false, name + " 应当读取失败，但构造成功");  // 如果没抛异常，说明损坏没有被检出。
    } catch (const std::runtime_error& error) {  // 捕获读取器主动抛出的错误。
        const std::string message = error.what();  // 取出人类可读的错误原因。
        check(message.find(expected_substring) != std::string::npos, name + " 的错误信息应包含 '" + expected_substring + "'，实际为: " + message);  // 关键词必须出现在错误信息里。
    }
}

// mutate：复制一份字节并把指定偏移改成新值，用于快速构造字段损坏样例。
std::vector<unsigned char> mutate(const std::vector<unsigned char>& base, std::size_t offset, unsigned char value) {
    std::vector<unsigned char> bytes = base;  // 复制模板，避免修改原始合法文件。
    bytes[offset] = value;  // 把指定偏移的字节改成新值。
    return bytes;  // 返回修改后的副本。
}

// expect_writer_error：断言某条非法记录会让 TraceWriter 抛错，且错误信息包含关键词。
void expect_writer_error(const std::filesystem::path& dir, const std::string& name, const memwhy::TraceRecord& record, const std::string& expected_substring) {
    const std::filesystem::path path = dir / name;  // 非法样例的输出路径。
    try {  // TraceWriter 应当在 write() 时拒绝非法记录。
        memwhy::TraceWriter writer(path.string());  // 创建写入器（此时文件可以正常打开）。
        writer.write(record);  // 尝试写入非法记录，预期抛错。
        writer.finish();  // 如果不抛错会执行到这里，说明校验缺失。
        check(false, name + " 应当写入失败，但写入了");  // 没有抛错就记为测试失败。
    } catch (const std::runtime_error& error) {  // 捕获写侧主动抛出的错误。
        const std::string message = error.what();  // 取出错误原因。
        check(message.find(expected_substring) != std::string::npos, name + " 的错误信息应包含 '" + expected_substring + "'，实际为: " + message);  // 错误信息必须包含预期关键词。
    }
}

// test_corruption_cases：构造工单要求的五种损坏，另加 kind/width 两种字段损坏。
void test_corruption_cases(const std::filesystem::path& dir) {
    const std::filesystem::path valid_path = dir / "valid.bin";  // 先写一份合法文件作为损坏模板。
    const std::vector<memwhy::TraceRecord> base_records = make_two_record_fixture();  // 复用公共的两记录夹具。
    write_trace(valid_path, base_records);  // 写出合法模板文件。
    const std::vector<unsigned char> valid = read_bytes(valid_path);  // 读成字节，便于复制修改。
    std::vector<unsigned char> truncated = valid;  // 复制一份做截断样例。
    truncated.resize(truncated.size() - 12);  // 砍掉最后 12 字节（半条记录）。
    expect_reader_error(dir, "truncated.bin", truncated, "size mismatch");  // 截断必须报大小不匹配。
    expect_reader_error(dir, "count_too_large.bin", mutate(valid, 8, static_cast<unsigned char>(valid[8] + 1)), "size mismatch");  // 头部记录数从 2 改成 3，文件大小不够必须报错。
    expect_reader_error(dir, "bad_reserved.bin", mutate(valid, 16 + 20, 1), "reserved");  // 第 1 条记录 offset 20 的 reserved 改成 1。
    expect_reader_error(dir, "bad_magic.bin", mutate(valid, 0, 'X'), "bad magic");  // 首字节从 'M' 改成 'X'。
    expect_reader_error(dir, "bad_version.bin", mutate(valid, 4, 2), "unsupported version");  // 版本字段改成 2。
    expect_reader_error(dir, "bad_kind.bin", mutate(valid, 16 + 4, 9), "invalid kind");  // 第 1 条 kind 改成 9。
    expect_reader_error(dir, "zero_width.bin", mutate(valid, 16 + 16, 0), "zero width");  // 第 1 条 width 改成 0。
}

// test_writer_rejects_invalid_records：写侧也要拒绝 kind/width/reserved 非法的记录。
void test_writer_rejects_invalid_records(const std::filesystem::path& dir) {
    memwhy::TraceRecord bad_kind;  // 构造一条非法记录。
    bad_kind.kind = 9;  // kind 只允许 0/1/2。
    bad_kind.width = 8;  // 其它字段保持合法，确保失败原因来自 kind。
    expect_writer_error(dir, "writer_bad_kind.bin", bad_kind, "kind");  // 预期错误信息提到 kind。
    memwhy::TraceRecord zero_width;  // 构造零宽度记录。
    zero_width.kind = 1;  // 合法的读访问类型。
    zero_width.width = 0;  // 宽度必须大于 0。
    expect_writer_error(dir, "writer_zero_width.bin", zero_width, "width");  // 预期错误信息提到 width。
    memwhy::TraceRecord bad_reserved;  // 构造保留字段非零记录。
    bad_reserved.kind = 1;  // 合法的读访问类型。
    bad_reserved.width = 8;  // 合法宽度。
    bad_reserved.reserved = 1;  // v1 保留字段必须为 0。
    expect_writer_error(dir, "writer_bad_reserved.bin", bad_reserved, "reserved");  // 预期错误信息提到 reserved。
}

// test_uniform_below：验证固定种子采样函数可复现且不越界。
void test_uniform_below() {
    std::mt19937_64 first_rng(7);  // 第一个引擎，种子 7。
    std::mt19937_64 second_rng(7);  // 第二个引擎，同样种子 7。
    for (int i = 0; i < 1000; ++i) {  // 连续抽取 1000 次做比较。
        const std::uint64_t first_value = memwhy::uniform_below(first_rng, 1000);  // 第一路取 [0,1000) 的随机数。
        const std::uint64_t second_value = memwhy::uniform_below(second_rng, 1000);  // 第二路取同样范围的随机数。
        check(first_value == second_value, "同种子的 uniform_below 序列必须一致");  // 同种子两次必须得到相同序列。
        check(first_value < 1000, "uniform_below 的返回值必须小于上界");  // 结果必须落在 [0, bound) 内。
    }
    std::mt19937_64 one_rng(9);  // 再用另一个种子测试上界为 1 的边界情况。
    check(memwhy::uniform_below(one_rng, 1) == 0, "上界为 1 时 uniform_below 只能返回 0");  // [0,1) 唯一合法值就是 0。
}

}  // 匿名命名空间结束。

int main() {  // 测试程序入口。
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "memwhy_trace_tests";  // 用系统临时目录下的固定子目录，避免污染仓库。
    std::error_code error;  // 接收文件系统操作的错误码，不让删除失败直接抛异常。
    std::filesystem::remove_all(temp_dir, error);  // 先清掉上次可能残留的测试目录。
    std::filesystem::create_directories(temp_dir, error);  // 创建干净的测试目录。
    test_micro_example(temp_dir);  // 执行微例测试。
    test_roundtrip(temp_dir);  // 执行往返测试。
    test_u64_address_roundtrip(temp_dir);  // 执行 64 位地址往返测试。
    test_same_seed_byte_identical(temp_dir);  // 执行同种子逐字节复现测试。
    test_corruption_cases(temp_dir);  // 执行损坏输入测试。
    test_writer_rejects_invalid_records(temp_dir);  // 执行写侧非法记录拒绝测试。
    test_uniform_below();  // 执行固定种子采样测试。
    std::filesystem::remove_all(temp_dir, error);  // 测试结束后清理临时目录。
    if (failures == 0) {  // 所有检查都通过。
        std::cout << "trace_tests: all checks passed\n";  // 打印成功信息，便于人工确认。
        return 0;  // 退出码 0 表示测试成功。
    }
    std::cerr << "trace_tests: " << failures << " check(s) failed\n";  // 打印失败总数。
    return 1;  // 退出码 1 让 CTest 标记失败。
}
