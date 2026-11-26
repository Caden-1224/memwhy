// tests/gen_tests.cpp — 合成轨迹生成器（chain）的 CTest 自检程序。
// 测试通过 CLI 驱动 `memwhy gen --kind chain`，用 TraceReader 读回轨迹，
// 断言地址依赖、单环覆盖、同种子逐字节复现与非法参数报错。
#include "chain.hpp"  // 引入 build_chain，用于按同种子重建 next 数组核对依赖。
#include "trace.hpp"  // 引入 TraceReader，用于读回并校验生成的轨迹文件。

#include <cstddef>     // 引入 std::size_t，表示记录下标与数量。
#include <cstdint>     // 引入 std::uint32_t/std::uint64_t，保证地址与字段宽度固定。
#include <cstdio>      // 提供 std::remove 等 C 文件函数。
#include <cstdlib>     // 提供 std::system，执行 CLI 子进程。
#include <cstring>     // 提供 std::strcmp，比较命令行参数。
#include <filesystem>  // 提供临时目录创建、删除与文件存在检查。
#include <fstream>     // 提供 std::ifstream，逐字节读取轨迹文件。
#include <iostream>    // 提供 std::cout/std::cerr，打印测试结果。
#include <iterator>    // 提供 std::istreambuf_iterator，一次读完整文件。
#include <string>      // 提供 std::string，拼装命令与错误信息。
#include <vector>      // 提供 std::vector，保存读回的记录与字节。

#include <sys/wait.h>  // 提供 WIFEXITED/WEXITSTATUS，解析 std::system 的退出状态。

namespace {  // 匿名命名空间：测试辅助函数只在本文件可见。

int failures = 0;  // 失败计数器：每个失败检查加一，最后决定进程退出码。

// check：断言一个条件成立；失败时打印原因并累计失败次数。
void check(bool condition, const std::string& what) {  // condition 为真是通过。
    if (!condition) {  // 条件不成立说明预期没有满足。
        std::cerr << "FAIL: " << what << '\n';  // 把失败原因写到标准错误，CTest 日志可见。
        ++failures;  // 累计失败数。
    }
}

// shell_quote：给路径加单引号，避免空格或特殊字符破坏 shell 命令。
std::string shell_quote(const std::string& text) {  // 输入原始字符串。
    std::string out = "'";  // 开头单引号。
    for (const char c : text) {  // 逐字符处理。
        if (c == '\'') {  // 单引号本身需要特殊处理。
            out += "'\\''";  // 结束当前引号、转义单引号、重新开引号。
        } else {  // 普通字符。
            out.push_back(c);  // 原样放入。
        }
    }
    out += "'";  // 结尾单引号。
    return out;  // 返回可直接拼进 shell 命令的字符串。
}

// read_text：读取文本文件；打不开时返回空串，便于检查错误输出。
std::string read_text(const std::filesystem::path& path) {  // 输入文件路径。
    std::ifstream input(path, std::ios::binary);  // 二进制模式打开。
    if (!input) {  // 文件不存在或不可读。
        return std::string();  // 返回空串。
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());  // 一次读完。
}

// read_bytes：把文件完整读成字节数组，用于逐字节比较。
std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {  // 输入文件路径。
    std::ifstream input(path, std::ios::binary);  // 二进制模式打开。
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());  // 用迭代器范围一次读完。
}

// CommandResult 保存一次命令执行的退出码与输出。
struct CommandResult {  // 简单的命令结果结构。
    int exit_code = -1;  // 进程退出码；-1 表示 std::system 本身失败。
    std::string out;     // 标准输出内容。
    std::string err;     // 标准错误内容。
};

// run_capture：执行 shell 命令，把 stdout/stderr 重定向到日志文件后读回。
CommandResult run_capture(const std::string& command, const std::filesystem::path& log_dir, const std::string& tag) {  // tag 用于区分不同调用的日志文件名。
    const std::filesystem::path out_path = log_dir / (tag + ".out");  // 标准输出日志文件。
    const std::filesystem::path err_path = log_dir / (tag + ".err");  // 标准错误日志文件。
    const std::string full = command + " >" + shell_quote(out_path.string()) + " 2>" + shell_quote(err_path.string());  // 拼出带重定向的完整命令。
    const int status = std::system(full.c_str());  // 交给 shell 执行。
    CommandResult result;  // 准备返回结果。
    if (status == -1) {  // std::system 无法启动 shell。
        result.exit_code = -1;  // 保留 -1 表示环境错误。
    } else if (WIFEXITED(status)) {  // 正常退出。
        result.exit_code = WEXITSTATUS(status);  // 取出真实退出码。
    } else if (WIFSIGNALED(status)) {  // 被信号终止。
        result.exit_code = 128 + WTERMSIG(status);  // 按 shell 惯例换算成 128+信号编号。
    }
    result.out = read_text(out_path);  // 读回标准输出。
    result.err = read_text(err_path);  // 读回标准错误。
    return result;  // 返回结果。
}

// gen_command：拼出一条 `memwhy gen` 命令；参数由调用方给出原始片段。
std::string gen_command(const std::string& exe, const std::string& args) {  // exe 是 memwhy 可执行文件路径。
    return shell_quote(exe) + " gen " + args;  // gen 子命令后直接拼参数。
}

// read_records：用 TraceReader 读出一个轨迹文件的全部记录。
std::vector<memwhy::TraceRecord> read_records(const std::filesystem::path& path) {  // 输入轨迹路径。
    memwhy::TraceReader reader(path.string());  // 构造读取器，自动完成全部格式校验。
    std::vector<memwhy::TraceRecord> records;  // 保存读出的记录。
    memwhy::TraceRecord record;  // 临时记录变量。
    while (reader.next(record)) {  // 一直读到文件末尾。
        records.push_back(record);  // 追加到结果列表。
    }
    return records;  // 返回全部记录。
}

// test_chain_dependency：验证 CLI 生成的轨迹满足“下一条地址由当前节点 next 决定”。
void test_chain_dependency(const std::string& exe, const std::filesystem::path& dir) {  // exe 是 CLI 路径。
    const std::filesystem::path out = dir / "chain_dependency.bin";  // 输出轨迹路径。
    const std::string args = "--kind chain --seed 42 --out " + shell_quote(out.string()) + " --working-set 256 --chain-len 12 --node-size 64 --base 4096 --core 2";  // 4 节点、12 步的链参数。
    const CommandResult result = run_capture(gen_command(exe, args), dir, "chain_dependency");  // 执行生成命令。
    check(result.exit_code == 0, "合法 chain 参数应以退出码 0 结束，实际错误: " + result.err);  // 命令必须成功。
    const std::vector<memwhy::TraceRecord> records = read_records(out);  // 读回轨迹，格式校验通过才继续。
    check(records.size() == 12, "chain-len=12 应生成 12 条记录");  // 记录数必须等于链长。
    std::uint32_t node = 0;  // 保存按同种子重建出的入口节点。
    const std::vector<std::uint32_t> next = memwhy::build_chain(42, 4, &node);  // 按同种子重建 4 节点 next 数组并取得 perm[0]。
    for (std::size_t index = 0; index < records.size(); ++index) {  // 逐条核对记录内容与依赖关系。
        const memwhy::TraceRecord& record = records[index];  // 当前记录。
        check(record.core == 2, "记录 core 应等于 --core 参数");  // 核心号必须透传。
        check(record.kind == 1, "链访问应为读（kind=1）");  // 指针链是依赖读。
        check(record.width == 8, "指针宽度应为 8 字节");  // 指针宽度固定 8。
        const std::uint64_t expected_addr = 4096 + static_cast<std::uint64_t>(node) * 64;  // 当前节点对应的字节地址。
        check(record.addr == expected_addr, "第 " + std::to_string(index) + " 条地址应等于 base + node*node_size");  // 地址必须与节点一致。
        node = next[node];  // 沿同一条链前进到下一节点。
    }
}

// test_single_ring_via_cli：链长超过节点数时按单环循环，前 N 条恰好覆盖全部节点一次。
void test_single_ring_via_cli(const std::string& exe, const std::filesystem::path& dir) {  // exe 是 CLI 路径。
    const std::filesystem::path out = dir / "chain_ring.bin";  // 输出轨迹路径。
    const std::string args = "--kind chain --seed 7 --out " + shell_quote(out.string()) + " --working-set 256 --chain-len 10 --node-size 64";  // 4 节点、10 步，必然绕环两圈半。
    const CommandResult result = run_capture(gen_command(exe, args), dir, "chain_ring");  // 执行生成命令。
    check(result.exit_code == 0, "单环测试命令应成功，实际错误: " + result.err);  // 命令必须成功。
    const std::vector<memwhy::TraceRecord> records = read_records(out);  // 读回轨迹。
    check(records.size() == 10, "chain-len=10 应生成 10 条记录");  // 记录数必须正确。
    std::vector<std::uint64_t> first_nodes;  // 前 4 条记录的节点号（用地址除以 64 还原）。
    for (std::size_t index = 0; index < 4 && index < records.size(); ++index) {  // 只看第一圈。
        first_nodes.push_back(records[index].addr / 64);  // 地址都是 64 的倍数，直接除回节点号。
    }
    check(first_nodes.size() == 4, "应能取到第一圈的 4 个节点");  // 长度检查，避免后面越界。
    std::vector<int> seen(4, 0);  // 统计第一圈每个节点出现次数。
    for (const std::uint64_t node : first_nodes) {  // 遍历第一圈节点。
        if (node < 4) {  // 防御性检查节点号合法。
            ++seen[static_cast<std::size_t>(node)];  // 计数。
        }
    }
    bool covered_once = true;  // 先假设恰好覆盖一次。
    for (const int count : seen) {  // 检查每个节点。
        if (count != 1) {  // 不是 1 说明有短环或漏节点。
            covered_once = false;  // 记为失败。
        }
    }
    check(covered_once, "chain_len > N 时第一圈应恰好覆盖 4 个节点各一次");  // 单环性质必须成立。
    for (std::size_t index = 4; index < records.size(); ++index) {  // 检查后续圈是否按 4 步周期重复。
        check(records[index].addr == records[index - 4].addr, "超过节点数后地址应按单环周期重复");  // 第 k 条与第 k-4 条必须相同。
    }
}

// test_same_seed_byte_identical：同参数两次生成必须逐字节一致。
void test_same_seed_byte_identical(const std::string& exe, const std::filesystem::path& dir) {  // exe 是 CLI 路径。
    const std::filesystem::path first = dir / "chain_seed_a.bin";  // 第一次输出路径。
    const std::filesystem::path second = dir / "chain_seed_b.bin";  // 第二次输出路径。
    const std::string base_args = "--kind chain --seed 123 --working-set 4096 --chain-len 200 --node-size 64";  // 两次共用的参数。
    const CommandResult first_result = run_capture(gen_command(exe, base_args + " --out " + shell_quote(first.string())), dir, "chain_seed_a");  // 第一次生成。
    const CommandResult second_result = run_capture(gen_command(exe, base_args + " --out " + shell_quote(second.string())), dir, "chain_seed_b");  // 第二次生成。
    check(first_result.exit_code == 0 && second_result.exit_code == 0, "两次同种子生成都应成功");  // 两次都必须成功。
    check(read_bytes(first) == read_bytes(second), "同种子两次生成的轨迹必须逐字节一致");  // 固定种子纪律的核心断言。
}

// test_seed_changes_sequence：不同种子应得到不同排列（用 64 节点降低碰撞概率）。
void test_seed_changes_sequence(const std::string& exe, const std::filesystem::path& dir) {  // exe 是 CLI 路径。
    const std::filesystem::path out_a = dir / "chain_diff_a.bin";  // 种子 A 输出。
    const std::filesystem::path out_b = dir / "chain_diff_b.bin";  // 种子 B 输出。
    const std::string common = "--kind chain --working-set 4096 --chain-len 64 --node-size 64";  // 共用参数。
    const CommandResult result_a = run_capture(gen_command(exe, common + " --seed 1 --out " + shell_quote(out_a.string())), dir, "chain_diff_a");  // 种子 1。
    const CommandResult result_b = run_capture(gen_command(exe, common + " --seed 2 --out " + shell_quote(out_b.string())), dir, "chain_diff_b");  // 种子 2。
    check(result_a.exit_code == 0 && result_b.exit_code == 0, "不同种子生成都应成功");  // 两次都必须成功。
    check(read_bytes(out_a) != read_bytes(out_b), "不同种子应生成不同的链轨迹");  // 64 节点下碰撞可以忽略。
}

// test_invalid_parameters：非法参数必须报错退出，且不产出轨迹文件。
void test_invalid_parameters(const std::string& exe, const std::filesystem::path& dir) {  // exe 是 CLI 路径。
    struct Case {  // 单个非法参数用例。
        std::string name;  // 用例名，用于日志文件名。
        std::string args;  // 传给 gen 的参数片段。
        std::string keyword;  // 错误信息中应出现的关键词。
    };
    const Case cases[] = {  // 覆盖任务点名的非法参数与常见缺参。
        {"not_divisible", "--kind chain --seed 1 --working-set 100 --chain-len 10 --node-size 64", "divisible"},  // 工作集不是节点大小整数倍。
        {"node_too_small", "--kind chain --seed 1 --working-set 256 --chain-len 10 --node-size 32", "node_size"},  // 节点小于缓存行。
        {"node_zero", "--kind chain --seed 1 --working-set 256 --chain-len 10 --node-size 0", "node_size"},  // 节点大小为 0。
        {"chain_len_zero", "--kind chain --seed 1 --working-set 256 --chain-len 0 --node-size 64", "chain_len"},  // 链长为 0。
        {"working_set_zero", "--kind chain --seed 1 --working-set 0 --chain-len 10 --node-size 64", "working_set"},  // 工作集为 0。
        {"missing_seed", "--kind chain --working-set 256 --chain-len 10 --node-size 64", "seed"},  // 缺少种子。
        {"unknown_option", "--kind chain --seed 1 --working-set 256 --chain-len 10 --bogus 1", "bogus"},  // 未知参数。
    };
    for (const Case& item : cases) {  // 逐个执行非法用例。
        const std::filesystem::path out = dir / ("invalid_" + item.name + ".bin");  // 每个用例分配独立输出路径。
        const std::string args = item.args + " --out " + shell_quote(out.string());  // 补上输出路径。
        const CommandResult result = run_capture(gen_command(exe, args), dir, "invalid_" + item.name);  // 执行命令。
        check(result.exit_code != 0, "非法参数 " + item.name + " 必须以非零退出码失败");  // 必须报错。
        check(result.err.find(item.keyword) != std::string::npos, "非法参数 " + item.name + " 的错误信息应包含 '" + item.keyword + "'");  // 错误信息要能定位问题。
        check(!std::filesystem::exists(out), "非法参数 " + item.name + " 不应产出轨迹文件");  // 失败时不能留下半成品。
    }
}

// test_chain_len_less_than_nodes：链长小于节点数时只触达部分工作集，且 stdout 报告实际触达数。
void test_chain_len_less_than_nodes(const std::string& exe, const std::filesystem::path& dir) {  // exe 是 CLI 路径。
    const std::filesystem::path out = dir / "chain_short.bin";  // 输出轨迹路径。
    const std::string args = "--kind chain --seed 9 --out " + shell_quote(out.string()) + " --working-set 512 --chain-len 3 --node-size 64";  // 8 个节点只走 3 步。
    const CommandResult result = run_capture(gen_command(exe, args), dir, "chain_short");  // 执行生成命令。
    check(result.exit_code == 0, "短链参数应成功，实际错误: " + result.err);  // 命令必须成功。
    check(result.out.find("3 distinct nodes") != std::string::npos, "stdout 应报告实际触达 3 个不同节点");  // 触达数口径必须可见。
    const std::vector<memwhy::TraceRecord> records = read_records(out);  // 读回轨迹。
    check(records.size() == 3, "chain-len=3 应生成 3 条记录");  // 记录数必须等于链长。
    std::vector<std::uint64_t> addresses;  // 保存 3 条记录的地址。
    for (const memwhy::TraceRecord& record : records) {  // 遍历记录做范围与唯一性检查。
        check(record.addr < 512, "短链地址必须落在工作集范围内");  // 地址不能超出 512 字节工作集。
        check(record.addr % 64 == 0, "短链地址必须按 64 字节节点对齐");  // 地址必须落在节点边界。
        for (const std::uint64_t existing : addresses) {  // 与已收集地址比较。
            check(existing != record.addr, "chain_len < N 时不应重复访问同一节点");  // 单环性质保证第一圈不重复。
        }
        addresses.push_back(record.addr);  // 收集当前地址。
    }
}

// test_default_node_size：不显式给 --node-size 时应默认 64，地址按 64 对齐。
void test_default_node_size(const std::string& exe, const std::filesystem::path& dir) {  // exe 是 CLI 路径。
    const std::filesystem::path out = dir / "chain_default_node.bin";  // 输出轨迹路径。
    const CommandResult result = run_capture(gen_command(exe, "--kind chain --seed 5 --working-set 256 --chain-len 8 --out " + shell_quote(out.string())), dir, "chain_default_node");  // 省略 --node-size。
    check(result.exit_code == 0, "省略 --node-size 时应成功，实际错误: " + result.err);  // 默认值必须生效。
    const std::vector<memwhy::TraceRecord> records = read_records(out);  // 读回轨迹。
    bool aligned = true;  // 先假设所有地址都按 64 对齐。
    for (const memwhy::TraceRecord& record : records) {  // 遍历记录。
        if (record.addr % 64 != 0) {  // 地址不是 64 的倍数说明默认值不对。
            aligned = false;  // 记为失败。
        }
    }
    check(aligned, "默认 node_size=64 时所有地址应 64 字节对齐");  // 默认值断言。
}

}  // 匿名命名空间结束。

int main(int argc, char** argv) {  // 测试程序入口；argv[1] 是 memwhy 可执行文件路径。
    if (argc < 2) {  // 没有可执行文件路径就无法驱动 CLI。
        std::cerr << "usage: gen_tests <path-to-memwhy>\n";  // 提示正确用法。
        return 2;  // 用法错误退出码。
    }
    const std::string exe = argv[1];  // 保存 CLI 路径。
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "memwhy_gen_tests";  // 用系统临时目录下的固定子目录。
    std::error_code error;  // 接收文件系统操作的错误码。
    std::filesystem::remove_all(temp_dir, error);  // 先清掉上次可能残留的目录。
    std::filesystem::create_directories(temp_dir, error);  // 创建干净的测试目录。
    test_chain_dependency(exe, temp_dir);  // 验证地址依赖。
    test_single_ring_via_cli(exe, temp_dir);  // 验证单环覆盖与周期。
    test_same_seed_byte_identical(exe, temp_dir);  // 验证同种子逐字节复现。
    test_seed_changes_sequence(exe, temp_dir);  // 验证不同种子产生不同序列。
    test_invalid_parameters(exe, temp_dir);  // 验证非法参数报错。
    test_chain_len_less_than_nodes(exe, temp_dir);  // 验证短链只触达部分工作集。
    test_default_node_size(exe, temp_dir);  // 验证默认节点大小。
    std::filesystem::remove_all(temp_dir, error);  // 测试结束后清理临时目录。
    if (failures == 0) {  // 所有检查通过。
        std::cout << "gen_tests: all checks passed\n";  // 打印成功信息。
        return 0;  // 退出码 0 表示成功。
    }
    std::cerr << "gen_tests: " << failures << " check(s) failed\n";  // 打印失败总数。
    return 1;  // 退出码 1 让 CTest 标记失败。
}