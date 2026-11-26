// src/chain.hpp — 依赖指针链的构造接口。
// 本组件只负责“节点之间怎么连”，不接触文件、不接触 TraceRecord；
// 生成器用它发轨迹，板端测量工具用它向真实内存填指针，两边共用同一套链构造。
#pragma once  // 防止同一编译单元重复包含本头文件，避免函数声明重复。

#include <cstddef>  // 引入 std::size_t，表示节点数量。
#include <cstdint>  // 引入 std::uint32_t/std::uint64_t，保证节点索引与种子宽度固定。
#include <vector>   // 引入 std::vector，保存排列与 next 数组。

namespace memwhy {  // 项目命名空间，避免与第三方库符号重名。

// build_chain 生成一条覆盖 n_nodes 个节点的单一环，返回 next 数组。
// next[i] 是节点 i 的后继节点号；从任意节点出发沿 next 走 n_nodes 步会回到起点，
// 途中恰好经过每个节点一次。节点 i 的字节地址按 base + i * node_size 计算。
// 若 start 非空，函数会把洗牌后的 perm[0] 写入 *start，作为轨迹发射的入口节点；
// next 数组本身与起点无关，不关心顺序的调用方可以传 nullptr 并从任意节点开始。
// seed 相同则结果逐项相同（std::mt19937_64 + 手写无偏采样，跨标准库可复现）。
std::vector<std::uint32_t> build_chain(std::uint64_t seed, std::size_t n_nodes, std::uint32_t* start = nullptr);

// build_chain_from_perm 把给定的节点顺序串成一条单环，返回 next 数组。
// perm 必须恰好包含 0..N-1 各一次；该函数不碰随机数，供固定排列测试与需要显式顺序的调用方复用。
std::vector<std::uint32_t> build_chain_from_perm(const std::vector<std::uint32_t>& perm);

}  // MemWhy 命名空间结束。
