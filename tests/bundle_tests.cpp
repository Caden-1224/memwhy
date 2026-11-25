// tests/bundle_tests.cpp — Bundle 打包、校验与原子重命名的 CTest 自检程序。
// 测试通过 CLI 驱动 `memwhy bundle pack` 与 `memwhy bundle verify`，并直接读取 manifest 断言窗口字段。
#include "bundle.hpp"   // 引入 Bundle 公开接口，用于读取 manifest 字段。
#include "sha256.hpp"   // 引入 SHA-256，用于已知向量测试。
#include "trace.hpp"    // 引入 TraceWriter，生成可复现的两记录轨迹。

#include <algorithm>   // 提供 std::sort，把目录成员按字典序排列后比较。
#include <cstddef>     // 提供 std::size_t，表示下标与长度。
#include <cstdint>     // 提供 std::uint8_t，表示字节。
#include <cstdio>      // 提供 std::remove 等 C 文件函数。
#include <cstdlib>     // 提供 std::system/setenv/unsetenv。
#include <cstring>     // 提供 std::strcmp，比较参数。
#include <filesystem>  // 提供目录遍历、复制与删除。
#include <fstream>     // 提供 std::ifstream/std::ofstream。
#include <iostream>    // 提供 std::cout/std::cerr。
#include <iterator>    // 提供 std::istreambuf_iterator，一次读完整文件。
#include <string>      // 提供 std::string。
#include <vector>      // 提供 std::vector。

#include <sys/wait.h>  // 提供 WIFEXITED/WEXITSTATUS，解析 std::system 的退出状态。
#include <unistd.h>    // 提供 POSIX 环境变量与进程相关声明。

namespace {  // 匿名命名空间：测试辅助函数只在本文件可见。

int failures = 0;  // 失败计数器：最后决定进程退出码。

// check：断言一个条件成立；失败时打印原因并累计失败次数。
void check(bool condition, const std::string& what) {  // condition 为真是通过。
    if (!condition) {  // 条件不成立。
        std::cerr << "FAIL: " << what << '\n';  // 把失败原因写到标准错误。
        ++failures;  // 累计失败数。
    }
}

// read_bytes：把文件完整读成字节数组，用于逐字节比较。
std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {  // 输入文件路径。
    std::ifstream input(path, std::ios::binary);  // 二进制模式打开。
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());  // 用迭代器范围一次读完。
}

// write_bytes：把字节数组原样写回文件，用于制造损坏样例。
void write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {  // 目标路径与字节内容。
    std::ofstream output(path, std::ios::binary | std::ios::trunc);  // 二进制创建或覆盖。
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));  // 原样写出所有字节。
}

// read_text：读取文本文件；打不开时返回空串，便于检查错误输出文件是否存在。
std::string read_text(const std::filesystem::path& path) {  // 输入文件路径。
    std::ifstream input(path, std::ios::binary);  // 二进制模式打开。
    if (!input) {  // 文件不存在或不可读。
        return std::string();  // 返回空串。
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());  // 一次读完。
}

// write_text：把文本写到文件，用于准备 config/env 与损坏 manifest。
void write_text(const std::filesystem::path& path, const std::string& text) {  // 目标路径与文本内容。
    std::ofstream output(path, std::ios::binary | std::ios::trunc);  // 二进制创建或覆盖。
    output.write(text.data(), static_cast<std::streamsize>(text.size()));  // 写出全部字节。
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

// CommandResult 保存一次命令执行的退出码与输出。
struct CommandResult {
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

// write_two_record_trace：写出与工单微例相同的两记录轨迹。
void write_two_record_trace(const std::filesystem::path& path) {  // 输出路径。
    memwhy::TraceWriter writer(path.string());  // 创建轨迹写入器。
    memwhy::TraceRecord first;  // 第 1 条记录。
    first.core = 0;  // 核心 0。
    first.kind = 1;  // 读访问。
    first.addr = 0;  // 地址 0。
    first.width = 8;  // 8 字节。
    writer.write(first);  // 写入第 1 条。
    memwhy::TraceRecord second;  // 第 2 条记录。
    second.core = 0;  // 核心 0。
    second.kind = 2;  // 写访问。
    second.addr = 64;  // 地址 64。
    second.width = 8;  // 8 字节。
    writer.write(second);  // 写入第 2 条。
    writer.finish();  // 回填记录数并关闭文件。
}

// list_names：返回目录下所有条目的文件名，并按字典序排序。
std::vector<std::string> list_names(const std::filesystem::path& dir) {  // 输入目录。
    std::vector<std::string> names;  // 收集文件名。
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir)) {  // 遍历目录项。
        names.push_back(entry.path().filename().string());  // 只保留文件名，不带父目录。
    }
    std::sort(names.begin(), names.end());  // 排序，避免依赖文件系统返回顺序。
    return names;  // 返回排序后的名单。
}

// expect_verify_error：执行 CLI verify，断言失败且错误信息包含指定关键词。
void expect_verify_error(const std::string& cli, const std::filesystem::path& bundle, const std::string& expected, const std::string& tag, const std::filesystem::path& log_dir) {  // tag 用于日志文件名。
    const CommandResult result = run_capture(cli + " bundle verify --bundle " + shell_quote(bundle.string()), log_dir, tag);  // 运行校验命令。
    check(result.exit_code != 0, tag + " 应校验失败");  // 必须返回非零。
    check(result.err.find(expected) != std::string::npos, tag + " 的错误信息应包含 '" + expected + "'，实际: " + result.err);  // 错误信息必须点名原因。
}

// test_sha256_known_vectors：用 FIPS 180-4 的公开测试向量验证摘要实现。
void test_sha256_known_vectors() {  // 无参数。
    const std::string empty;  // 空消息。
    check(memwhy::sha256_hex(reinterpret_cast<const std::uint8_t*>(empty.data()), 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "空串 SHA-256 应符合标准向量");  // 空串标准摘要。
    const std::string abc = "abc";  // 三字节消息。
    check(memwhy::sha256_hex(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "'abc' SHA-256 应符合标准向量");  // 三字节标准摘要。
    const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 字节消息，触发补位到两组。
    check(memwhy::sha256_hex(reinterpret_cast<const std::uint8_t*>(two_blocks.data()), two_blocks.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "多块 SHA-256 应符合标准向量");  // 多块标准摘要。
}

// test_pack_and_verify：验证正常打包、CLI 校验、字节往返、manifest 字段与独立 sha256sum 结果。
void test_pack_and_verify(const std::string& cli, const std::filesystem::path& temp, const std::filesystem::path& trace, const std::filesystem::path& config) {  // 公共夹具。
    const std::filesystem::path bundle = temp / "basic";  // 输出 Bundle 目录。
    const std::string pack_command = cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string()) + " --out " + shell_quote(bundle.string());  // 打包命令。
    const CommandResult pack = run_capture(pack_command, temp, "pack_basic");  // 执行打包。
    check(pack.exit_code == 0, "pack 应成功: " + pack.err);  // 打包必须成功。
    const CommandResult verify = run_capture(cli + " bundle verify --bundle " + shell_quote(bundle.string()), temp, "verify_basic");  // 执行 CLI 校验。
    check(verify.exit_code == 0, "verify 应成功: " + verify.err);  // 校验必须成功。
    check(read_bytes(bundle / "trace.bin") == read_bytes(trace), "Bundle 内 trace.bin 应与输入逐字节一致");  // 轨迹字节往返。
    check(read_bytes(bundle / "config.json") == read_bytes(config), "Bundle 内 config.json 应与输入逐字节一致");  // 配置字节往返。
    const std::vector<std::string> expected_names = {"config.json", "manifest.json", "trace.bin"};  // 目录应恰有这三个文件。
    check(list_names(bundle) == expected_names, "Bundle 目录成员应恰为 config.json、manifest.json、trace.bin");  // 文件清单校验。
    const memwhy::BundleManifest manifest = memwhy::verify_bundle(bundle.string());  // 直接读回 manifest 做字段断言。
    check(manifest.members.size() == 2, "manifest.members 应恰含两项");  // 基础 Bundle 只有轨迹与配置。
    check(manifest.members[0].name == "trace.bin", "第一个成员应为 trace.bin");  // 成员顺序固定。
    check(manifest.members[1].name == "config.json", "第二个成员应为 config.json");  // 成员顺序固定。
    check(manifest.warmup == 0, "默认 warmup 应为 0");  // 默认预热为 0。
    check(manifest.measure_window.start == 0, "默认测量窗口起点应为 0");  // 默认从第 0 条开始。
    check(manifest.measure_window.count == 2, "默认测量窗口应覆盖全部 2 条记录");  // 默认覆盖全轨迹。
    const CommandResult sha_trace = run_capture("sha256sum " + shell_quote((bundle / "trace.bin").string()), temp, "sha_trace");  // 用独立工具计算轨迹摘要。
    check(sha_trace.exit_code == 0, "sha256sum 应可执行");  // 独立工具必须可用。
    check(sha_trace.out.rfind(manifest.members[0].sha256, 0) == 0, "trace.bin 的 sha256 应与 sha256sum 结果一致");  // 摘要必须匹配。
    const CommandResult sha_config = run_capture("sha256sum " + shell_quote((bundle / "config.json").string()), temp, "sha_config");  // 计算配置摘要。
    check(sha_config.out.rfind(manifest.members[1].sha256, 0) == 0, "config.json 的 sha256 应与 sha256sum 结果一致");  // 摘要必须匹配。
    const CommandResult repack = run_capture(pack_command, temp, "repack_existing");  // 再次向同一目录打包。
    check(repack.exit_code != 0, "输出目录已存在时 pack 应失败");  // 防止覆盖已有 Bundle。
    check(repack.err.find("already exists") != std::string::npos, "覆盖失败信息应包含 already exists");  // 错误信息应明确。
}

// test_same_input_same_manifest：同输入两次打包，manifest 必须逐字节一致。
void test_same_input_same_manifest(const std::string& cli, const std::filesystem::path& temp, const std::filesystem::path& trace, const std::filesystem::path& config) {  // 公共夹具。
    const std::filesystem::path first = temp / "same_a";  // 第一次输出。
    const std::filesystem::path second = temp / "same_b";  // 第二次输出。
    const std::string base = cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string());  // 公共命令前缀。
    check(run_capture(base + " --out " + shell_quote(first.string()), temp, "pack_same_a").exit_code == 0, "第一次打包应成功");  // 第一次打包。
    check(run_capture(base + " --out " + shell_quote(second.string()), temp, "pack_same_b").exit_code == 0, "第二次打包应成功");  // 第二次打包。
    check(read_text(first / "manifest.json") == read_text(second / "manifest.json"), "同输入两次打包的 manifest.json 应逐字节一致");  // 无时间戳与绝对路径。
}

// test_tamper_and_corruption：验证成员篡改、缺失成员与非法 manifest 都会被点名拒绝。
void test_tamper_and_corruption(const std::string& cli, const std::filesystem::path& temp) {  // 公共夹具。
    const std::filesystem::path source = temp / "basic";  // 使用基础 Bundle 作为模板。
    const std::filesystem::path tampered_trace = temp / "tampered_trace";  // 轨迹篡改副本。
    std::filesystem::copy(source, tampered_trace, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);  // 复制一份完整 Bundle。
    std::vector<unsigned char> trace_bytes = read_bytes(tampered_trace / "trace.bin");  // 读入轨迹字节。
    trace_bytes[20] ^= 0xffu;  // 翻转第 1 条记录内一个字节。
    write_bytes(tampered_trace / "trace.bin", trace_bytes);  // 写回篡改后的轨迹。
    expect_verify_error(cli, tampered_trace, "trace.bin", "verify_tampered_trace", temp);  // 错误必须点名 trace.bin。
    const std::filesystem::path tampered_config = temp / "tampered_config";  // 配置篡改副本。
    std::filesystem::copy(source, tampered_config, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);  // 复制完整 Bundle。
    std::vector<unsigned char> config_bytes = read_bytes(tampered_config / "config.json");  // 读入配置字节。
    config_bytes[0] ^= 0xffu;  // 翻转第一个字节。
    write_bytes(tampered_config / "config.json", config_bytes);  // 写回。
    expect_verify_error(cli, tampered_config, "config.json", "verify_tampered_config", temp);  // 错误必须点名 config.json。
    const std::filesystem::path missing_member = temp / "missing_member";  // 缺失成员副本。
    std::filesystem::copy(source, missing_member, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);  // 复制完整 Bundle。
    std::filesystem::remove(missing_member / "config.json");  // 删掉配置成员。
    expect_verify_error(cli, missing_member, "config.json", "verify_missing_member", temp);  // 错误必须点名 config.json。
    const std::filesystem::path bad_manifest = temp / "bad_manifest";  // 非法 manifest 副本。
    std::filesystem::copy(source, bad_manifest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);  // 复制完整 Bundle。
    write_text(bad_manifest / "manifest.json", "{not valid json");  // 写入非法 JSON。
    expect_verify_error(cli, bad_manifest, "manifest", "verify_bad_manifest", temp);  // 错误必须提到 manifest。
    const std::filesystem::path missing_manifest = temp / "missing_manifest";  // 缺失 manifest 副本。
    std::filesystem::copy(source, missing_manifest, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);  // 复制完整 Bundle。
    std::filesystem::remove(missing_manifest / "manifest.json");  // 删掉 manifest。
    expect_verify_error(cli, missing_manifest, "manifest.json", "verify_missing_manifest", temp);  // 错误必须点名 manifest.json。
}

// test_manifest_member_contract：验证成员数量与固定顺序被严格校验。
void test_manifest_member_contract(const std::string& cli, const std::filesystem::path& temp) {  // 公共夹具。
    const std::filesystem::path source = temp / "basic";  // 基础 Bundle 作为模板。
    const std::filesystem::path reversed = temp / "reversed_manifest";  // 成员顺序反转的副本。
    std::filesystem::copy(source, reversed, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);  // 复制完整 Bundle。
    const std::string trace_hash = memwhy::sha256_file_hex((reversed / "trace.bin").string());  // 重新计算轨迹摘要。
    const std::string config_hash = memwhy::sha256_file_hex((reversed / "config.json").string());  // 重新计算配置摘要。
    const std::string manifest = "{\n  \"members\": [\n    {\"name\": \"config.json\", \"sha256\": \"" + config_hash + "\"},\n    {\"name\": \"trace.bin\", \"sha256\": \"" + trace_hash + "\"}\n  ],\n  \"warmup\": 0,\n  \"measure_window\": {\"start\": 0, \"count\": 2}\n}\n";  // 故意反转两个成员。
    write_text(reversed / "manifest.json", manifest);  // 写入反转顺序的 manifest。
    expect_verify_error(cli, reversed, "first member must be trace.bin", "verify_reversed_members", temp);  // 必须报顺序错误。
    const std::filesystem::path extra = temp / "extra_file";  // 多余文件副本。
    std::filesystem::copy(source, extra, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);  // 复制完整 Bundle。
    write_text(extra / "extra.txt", "unexpected");  // 写入 manifest 未列出的文件。
    expect_verify_error(cli, extra, "unexpected file 'extra.txt'", "verify_extra_file", temp);  // 必须拒绝多余文件。
}

// test_env_bundle：验证 --env 会把环境快照纳入 manifest，篡改同样被点名。
void test_env_bundle(const std::string& cli, const std::filesystem::path& temp, const std::filesystem::path& trace, const std::filesystem::path& config, const std::filesystem::path& env) {  // 公共夹具。
    const std::filesystem::path bundle = temp / "with_env";  // 带环境快照的 Bundle。
    const CommandResult pack = run_capture(cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string()) + " --env " + shell_quote(env.string()) + " --out " + shell_quote(bundle.string()), temp, "pack_env");  // 执行打包。
    check(pack.exit_code == 0, "带 --env 的 pack 应成功: " + pack.err);  // 打包必须成功。
    const memwhy::BundleManifest manifest = memwhy::verify_bundle(bundle.string());  // 读取 manifest。
    check(manifest.members.size() == 3, "带 --env 时 manifest.members 应含三项");  // 轨迹、配置、环境快照。
    check(manifest.members[2].name == "env.json", "第三个成员应为 env.json");  // 成员顺序固定。
    check(read_bytes(bundle / "env.json") == read_bytes(env), "Bundle 内 env.json 应与输入逐字节一致");  // 环境快照往返。
    std::vector<unsigned char> env_bytes = read_bytes(bundle / "env.json");  // 读入环境快照字节。
    env_bytes[0] ^= 0xffu;  // 篡改第一个字节。
    write_bytes(bundle / "env.json", env_bytes);  // 写回篡改内容。
    expect_verify_error(cli, bundle, "env.json", "verify_tampered_env", temp);  // 错误必须点名 env.json。
    const std::filesystem::path missing_env = temp / "missing_env_input";  // 不存在的环境快照输入。
    const CommandResult missing = run_capture(cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string()) + " --env " + shell_quote(missing_env.string()) + " --out " + shell_quote((temp / "with_missing_env").string()), temp, "pack_missing_env");  // 尝试打包不存在的环境快照。
    check(missing.exit_code != 0, "--env 指向不存在文件时 pack 应失败");  // 必须拒绝非法输入。
    check(!std::filesystem::exists(temp / "with_missing_env"), "打包失败后不应留下目标目录");  // 失败必须原子。
}

// test_measure_window：验证 --warmup/--measure-start/--measure-count 写入 manifest。
void test_measure_window(const std::string& cli, const std::filesystem::path& temp, const std::filesystem::path& trace, const std::filesystem::path& config) {  // 公共夹具。
    const std::filesystem::path bundle = temp / "window";  // 自定义窗口 Bundle。
    const CommandResult pack = run_capture(cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string()) + " --warmup 1 --measure-start 0 --measure-count 1 --out " + shell_quote(bundle.string()), temp, "pack_window");  // 带窗口参数打包。
    check(pack.exit_code == 0, "带窗口参数的 pack 应成功: " + pack.err);  // 打包必须成功。
    const memwhy::BundleManifest manifest = memwhy::verify_bundle(bundle.string());  // 读取 manifest。
    check(manifest.warmup == 1, "warmup 应写成 1");  // 预热值。
    check(manifest.measure_window.start == 0, "measure_window.start 应写成 0");  // 起点。
    check(manifest.measure_window.count == 1, "measure_window.count 应写成 1");  // 长度。
    const std::filesystem::path invalid = temp / "invalid_window";  // 非法窗口输出目录。
    const CommandResult bad = run_capture(cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string()) + " --measure-start 3 --out " + shell_quote(invalid.string()), temp, "pack_invalid_window");  // 起点越过轨迹末尾。
    check(bad.exit_code != 0, "measure-start 越过轨迹末尾时 pack 应失败");  // 必须拒绝。
    check(!std::filesystem::exists(invalid), "非法窗口失败后不应留下目标目录");  // 失败必须原子。
    const std::filesystem::path negative = temp / "negative_warmup";  // 负数 warmup 输出目录。
    const CommandResult negative_result = run_capture(cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string()) + " --warmup -1 --out " + shell_quote(negative.string()), temp, "pack_negative_warmup");  // 尝试传负数。
    check(negative_result.exit_code != 0, "负数 warmup 应被拒绝");  // 必须失败。
    check(negative_result.err.find("invalid integer") != std::string::npos, "负数 warmup 的错误信息应包含 invalid integer，实际: " + negative_result.err);  // 必须是解析错误而非溢出后的语义错误。
}

// test_interrupted_pack：注入 rename 前失败，验证目标目录不存在且临时目录被清理。
void test_interrupted_pack(const std::string& cli, const std::filesystem::path& temp, const std::filesystem::path& trace, const std::filesystem::path& config) {  // 公共夹具。
    const std::filesystem::path target = temp / "interrupted";  // 中断场景目标目录。
    setenv("MEMWHY_TEST_PACK_FAIL_BEFORE_RENAME", "1", 1);  // 打开测试注入开关。
    const CommandResult pack = run_capture(cli + " bundle pack --trace " + shell_quote(trace.string()) + " --config " + shell_quote(config.string()) + " --out " + shell_quote(target.string()), temp, "pack_interrupted");  // 执行注定失败的打包。
    unsetenv("MEMWHY_TEST_PACK_FAIL_BEFORE_RENAME");  // 关闭注入开关，避免影响后续测试。
    check(pack.exit_code != 0, "注入 rename 前失败时 pack 应返回非零");  // 必须失败。
    check(!std::filesystem::exists(target), "中断后目标目录不应存在");  // 不允许半个 Bundle。
    bool temp_left = false;  // 是否存在残留临时目录。
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(temp)) {  // 遍历测试根目录。
        if (entry.path().filename().string().rfind("interrupted.tmp.", 0) == 0) {  // 找到本次使用的临时目录前缀。
            temp_left = true;  // 记录残留。
        }
    }
    check(!temp_left, "中断后不应留下临时目录");  // 临时目录必须被清理。
}

}  // 匿名命名空间结束。

int main(int argc, char** argv) {  // 测试入口；argv[1] 是 memwhy 可执行文件路径。
    if (argc < 2) {  // 必须由 CTest 传入 CLI 路径。
        std::cerr << "usage: bundle_tests /path/to/memwhy\n";  // 打印用法。
        return 2;  // 用法错误退出码。
    }
    const std::string cli = argv[1];  // 保存 CLI 路径。
    const std::filesystem::path temp = std::filesystem::temp_directory_path() / "memwhy_bundle_tests";  // 测试根目录。
    std::error_code error;  // 接收文件系统操作错误码。
    std::filesystem::remove_all(temp, error);  // 清掉上次残留。
    std::filesystem::create_directories(temp, error);  // 创建干净测试目录。
    const std::filesystem::path trace = temp / "input_trace.bin";  // 输入轨迹路径。
    write_two_record_trace(trace);  // 写出两记录轨迹。
    const std::filesystem::path config = temp / "input_config.json";  // 输入配置路径。
    std::string config_text;  // 配置内容。
    for (int i = 0; i < 200; ++i) {  // 构造 200 字节内容，覆盖多块 SHA-256。
        config_text.push_back(static_cast<char>('a' + (i % 26)));  // 循环使用 a-z。
    }
    write_text(config, config_text);  // 写出配置。
    const std::filesystem::path env = temp / "input_env.json";  // 环境快照路径。
    write_text(env, "{\"board\":\"test\"}\n");  // 写一个最小 JSON 环境快照。
    test_sha256_known_vectors();  // 先验证摘要实现本身。
    test_pack_and_verify(cli, temp, trace, config);  // 正常打包与校验。
    test_same_input_same_manifest(cli, temp, trace, config);  // 同输入逐字节一致。
    test_tamper_and_corruption(cli, temp);  // 篡改与损坏检出。
    test_manifest_member_contract(cli, temp);  // 成员顺序契约。
    test_env_bundle(cli, temp, trace, config, env);  // 环境快照。
    test_measure_window(cli, temp, trace, config);  // 测量窗口。
    test_interrupted_pack(cli, temp, trace, config);  // 原子失败。
    std::filesystem::remove_all(temp, error);  // 测试结束清理。
    if (failures == 0) {  // 全部通过。
        std::cout << "bundle_tests: all checks passed\n";  // 打印成功信息。
        return 0;  // 成功退出码。
    }
    std::cerr << "bundle_tests: " << failures << " check(s) failed\n";  // 打印失败总数。
    return 1;  // 失败退出码。
}