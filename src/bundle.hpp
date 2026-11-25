// src/bundle.hpp — Bundle 打包与校验的公开接口。
// Bundle 是复现单元：把轨迹、配置、可选环境快照和 manifest 绑成一个哈希保护的整体。
#pragma once  // 防止同一编译单元重复包含本头文件，避免结构体重复定义。

#include <cstdint>  // 引入 std::uint64_t，表示记录数与窗口下标。
#include <string>   // 引入 std::string，保存路径与成员名。
#include <vector>   // 引入 std::vector，保存成员列表。

namespace memwhy {  // 项目命名空间，避免与第三方库符号冲突。

// ManifestMember 表示 manifest 里的一个成员条目：文件名 + SHA-256 十六进制摘要。
struct ManifestMember {
    std::string name;    // 成员文件名，例如 trace.bin、config.json、env.json。
    std::string sha256;  // 成员文件字节的 SHA-256，64 个小写十六进制字符。
};

// MeasureWindow 表示测量窗口：从第 start 条记录开始、连续 count 条计入报告。
struct MeasureWindow {
    std::uint64_t start = 0;  // 起始记录下标，从 0 开始。
    std::uint64_t count = 0;  // 窗口内记录条数。
};

// BundleManifest 是 manifest.json 解析后的内存表示。
struct BundleManifest {
    std::vector<ManifestMember> members;  // 成员列表，顺序即 manifest 中的顺序。
    std::uint64_t warmup = 0;              // 预热记录数：回放时推进缓存状态但不计入报告。
    MeasureWindow measure_window;         // 测量窗口。
};

// BundlePackOptions 收集打包命令的全部参数，避免 pack_bundle 出现一长串位置参数。
struct BundlePackOptions {
    std::string trace_path;   // 输入轨迹文件路径。
    std::string config_path;  // 输入配置文件路径（按不透明字节处理）。
    std::string env_path;     // 可选环境快照路径；空字符串表示不打包 env.json。
    std::string out_dir;      // 输出 Bundle 目录路径。
    std::uint64_t warmup = 0;          // 预热记录数，默认 0。
    std::uint64_t measure_start = 0;   // 测量窗口起始下标，默认 0。
    std::uint64_t measure_count = 0;   // 测量窗口记录数；仅当 measure_count_set 为真时使用。
    bool measure_count_set = false;    // 是否显式传入 --measure-count；否则默认覆盖到轨迹末尾。
};

// pack_bundle：校验输入、写临时目录、计算哈希、写 manifest、fsync，最后原子重命名到 out_dir。
// 任何失败都会清理临时目录，目标目录要么完整出现、要么完全不存在。
void pack_bundle(const BundlePackOptions& options);

// verify_bundle：读取并校验 manifest，逐个重算成员哈希；失败时抛 std::runtime_error 并点名成员。
// 返回解析后的 manifest，供调用方继续读取 warmup 与测量窗口。
BundleManifest verify_bundle(const std::string& bundle_dir);

}  // namespace memwhy