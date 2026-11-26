// src/chain.cpp — 依赖指针链构造的实现。
// 核心是 Fisher–Yates 洗牌：先得到 0..N-1 的随机排列 perm，
// 再令 next[perm[i]] = perm[(i+1) % N]。这样每个节点恰好有一个前驱和一个后继，
// 整条链是一个单一环，不存在会被预取器或缓存掩盖的短循环。
#include "chain.hpp"  // 引入本模块的公开声明，保证定义与声明一致。

#include "random.hpp"  // 引入固定种子无偏采样函数 uniform_below。

#include <cstddef>   // 提供 std::size_t，用于节点数与循环下标。
#include <cstdint>   // 提供 std::uint32_t/std::uint64_t，保证字段宽度固定。
#include <limits>    // 提供 std::numeric_limits，检查节点数能否用 32 位索引表示。
#include <numeric>   // 提供 std::iota，把排列数组初始化为 0,1,2,...,N-1。
#include <stdexcept> // 提供 std::invalid_argument，参数非法时抛出明确异常。
#include <string>    // 提供 std::to_string，拼装错误信息。
#include <utility>   // 提供 std::swap，交换排列中的两个元素。
#include <vector>    // 提供 std::vector，保存排列与 next 数组。

namespace memwhy {  // 与头文件保持同一个命名空间。

std::vector<std::uint32_t> build_chain_from_perm(const std::vector<std::uint32_t>& perm) {  // 把给定排列串成单环。
    if (perm.empty()) {  // 空排列无法构成环。
        throw std::invalid_argument("chain: permutation must not be empty");  // 明确拒绝空输入。
    }
    std::vector<int> seen(perm.size(), 0);  // 记录每个节点在排列中出现次数。
    for (const std::uint32_t node : perm) {  // 逐个检查排列元素。
        if (node >= perm.size()) {  // 节点号必须落在 [0,N) 内。
            throw std::invalid_argument("chain: permutation value " + std::to_string(node) + " is out of range");  // 报告越界值。
        }
        ++seen[node];  // 统计该节点出现次数。
    }
    for (const int count : seen) {  // 检查每个节点的出现次数。
        if (count != 1) {  // 不是恰好一次说明有重复或缺失。
            throw std::invalid_argument("chain: permutation must contain every node exactly once");  // 报告非法排列。
        }
    }
    std::vector<std::uint32_t> next(perm.size());  // 准备 next 数组，长度与节点数相同。
    for (std::size_t i = 0; i < perm.size(); ++i) {  // 按排列顺序把相邻节点串起来。
        const std::size_t successor = (i + 1) % perm.size();  // 下一个排列位置；最后一个回到 0 形成闭环。
        next[perm[i]] = perm[successor];  // 节点 perm[i] 的后继是 perm[i+1]，保证单环。
    }
    return next;  // 返回构造好的 next 数组。
}

std::vector<std::uint32_t> build_chain(std::uint64_t seed, std::size_t n_nodes, std::uint32_t* start) {  // 构造一条 n_nodes 个节点的单环链。
    if (n_nodes == 0) {  // 没有节点的链没有意义，也可能让调用方除零。
        throw std::invalid_argument("chain: n_nodes must be > 0");  // 明确拒绝空链。
    }
    if (n_nodes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {  // next 数组元素是 32 位，节点数不能超过其上限。
        throw std::invalid_argument("chain: n_nodes exceeds 32-bit index range");  // 报告规模越界。
    }
    std::mt19937_64 rng(seed);  // 用固定种子初始化标准化的 64 位梅森旋转引擎。
    std::vector<std::uint32_t> perm(n_nodes);  // 准备保存 0..N-1 的排列。
    std::iota(perm.begin(), perm.end(), std::uint32_t{0});  // 初始顺序为 0,1,2,...,N-1。
    for (std::size_t i = n_nodes; i > 1; --i) {  // Fisher–Yates：从末尾向前，每次在 [0,i) 中选一个位置交换。
        const std::uint64_t j = uniform_below(rng, static_cast<std::uint64_t>(i));  // 在 [0,i) 内无偏取一个下标。
        std::swap(perm[i - 1], perm[static_cast<std::size_t>(j)]);  // 把选中的元素换到当前末尾位置。
    }
    if (start != nullptr) {  // 调用方需要知道轨迹从哪个节点开始。
        *start = perm[0];  // 洗牌后的第一个节点就是 perm[0]，按工单约定从它出发。
    }
    return build_chain_from_perm(perm);  // 复用纯排列构造，保证算法只有一份实现。
}

}  // MemWhy 命名空间结束。
