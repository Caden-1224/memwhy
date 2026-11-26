// src/gen.hpp — 合成轨迹生成器的公开接口。
// gen.cpp 负责 CLI 参数解析与 --kind 分发；gen_chain.cpp 只负责把链参数变成 v1 轨迹。
#pragma once  // 防止重复包含本头文件。

#include <cstdint>  // 引入 std::uint32_t/std::uint64_t，保证参数字段宽度固定。
#include <string>   // 引入 std::string，保存输出路径。

namespace memwhy {  // 项目命名空间。

// kGenUsageLine 是 gen 子命令的唯一用法文本；main 的总用法与 gen 的错误提示共用，避免两处漂移。
inline constexpr const char* kGenUsageLine =
    "       memwhy gen --kind chain --seed N --out PATH --working-set BYTES --chain-len N [--node-size 64] [--base 0] [--core 0]\n";  // 当前仅支持 chain。

// ChainGenOptions 收集 `memwhy gen --kind chain` 的全部参数。
struct ChainGenOptions {  // 只保留已落地参数，矩阵/stride 的参数留待后续扩展。
    std::string out_path;            // 输出轨迹文件路径；必填。
    std::uint64_t seed = 0;          // 随机种子；同种子同参数逐字节复现。
    std::uint64_t working_set = 0;   // 工作集字节数；必须能被 node_size 整除。
    std::uint64_t chain_len = 0;     // 生成的记录条数；超过节点数时按单环循环。
    std::uint64_t node_size = 64;    // 每个节点占用的字节数；默认 64（缓存行）。
    std::uint64_t base = 0;          // 工作集起始地址；默认 0。
    std::uint32_t core = 0;          // 记录中的核心号；默认 0。
};

// generate_chain 校验参数并生成依赖指针链轨迹；参数非法时抛 std::invalid_argument。
void generate_chain(const ChainGenOptions& options);

// run_gen 解析 `memwhy gen ...` 参数并执行；成功返回 0，参数错误抛异常由 main 统一处理。
int run_gen(int argc, char** argv);

}  // MemWhy 命名空间结束。
