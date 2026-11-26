// src/gen_chain.cpp — 把依赖指针链参数发射成轨迹 v1 文件。
// 本文件只做“参数校验 + 调链构造 + 逐条写 TraceWriter”，不参与 CLI 解析，
// 这样板端测量工具可以只复用 chain.hpp，不必依赖任何 CLI 代码。
#include "gen.hpp"  // 引入生成器参数结构与入口声明。

#include "chain.hpp"  // 引入 build_chain，构造单环。
#include "trace.hpp"  // 引入 TraceWriter，按轨迹 v1 布局写文件。

#include <cstddef>   // 提供 std::size_t，用于节点数与循环下标。
#include <cstdint>   // 提供 std::uint32_t/std::uint64_t，保证字段宽度固定。
#include <limits>    // 提供 std::numeric_limits，检查节点数与地址溢出。
#include <stdexcept> // 提供 std::invalid_argument，参数错误时抛出明确异常。
#include <string>    // 提供 std::string/std::to_string，拼装错误信息。
#include <vector>    // 提供 std::vector，保存 next 数组与节点序列。

namespace memwhy {  // 与头文件保持同一命名空间。

void generate_chain(const ChainGenOptions& options) {  // 生成一条依赖指针链轨迹。
    if (options.node_size < 64) {  // 小于缓存行的节点会让多个节点落在同一行，防预取前提不成立。
        throw std::invalid_argument("gen chain: node_size must be >= 64 bytes (cache line), got " + std::to_string(options.node_size));  // 报告非法节点大小。
    }
    if (options.working_set == 0) {  // 空工作集无法构造链。
        throw std::invalid_argument("gen chain: working_set must be > 0");  // 报告工作集为零。
    }
    if (options.working_set % options.node_size != 0) {  // 工作集必须刚好切成整数个节点。
        throw std::invalid_argument("gen chain: working_set must be divisible by node_size");  // 报告不能整除。
    }
    if (options.chain_len == 0) {  // 链长为零会生成空轨迹，没有实验意义。
        throw std::invalid_argument("gen chain: chain_len must be > 0");  // 报告链长为零。
    }
    const std::uint64_t node_count = options.working_set / options.node_size;  // 计算节点总数 N。
    // 节点数上限由 build_chain 统一校验：next 数组是 32 位索引，超过上限会直接抛错。
    if (node_count - 1 > (std::numeric_limits<std::uint64_t>::max() - options.base) / options.node_size) {  // 检查最后一个节点地址 base+(N-1)*node_size 不会溢出 64 位。
        throw std::invalid_argument("gen chain: base + working_set overflows 64-bit address space");  // 报告地址溢出。
    }
    std::uint32_t start = 0;  // 保存洗牌后的 perm[0]，作为链的入口节点。
    const std::vector<std::uint32_t> next = build_chain(options.seed, static_cast<std::size_t>(node_count), &start);  // 构造单环 next 数组并取得起点。
    TraceWriter writer(options.out_path);  // 打开输出文件并写占位头。
    std::uint32_t node = start;  // 按工单约定从 perm[0] 出发。
    for (std::uint64_t step = 0; step < options.chain_len; ++step) {  // 连续走 chain_len 步。
        TraceRecord record;  // 默认初始化一条 v1 记录（reserved 自动为 0）。
        record.core = options.core;  // 写入调用方指定的核心号。
        record.kind = 1;  // 依赖指针链的每一跳都是读访问。
        record.addr = options.base + static_cast<std::uint64_t>(node) * options.node_size;  // 节点地址 = base + 节点号 × 节点大小。
        record.width = 8;  // 指针宽度固定 8 字节。
        writer.write(record);  // 追加一条 24 字节记录。
        node = next[node];  // 沿单环走到下一个节点，形成真实地址依赖。
    }
    writer.finish();  // 回填记录数并关闭文件。
}

}  // MemWhy 命名空间结束。
