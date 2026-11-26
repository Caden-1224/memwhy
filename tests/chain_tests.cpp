// tests/chain_tests.cpp — 依赖指针链构造组件的 CTest 自检程序。
// 本文件只验证 chain.hpp 的公开行为：微例地址序列、单环性质与同种子可复现，
// 不依赖任何内部实现细节，后续板端测量工具复用同一接口。
#include "chain.hpp"  // 引入被测的链构造公开接口。

#include <cstddef>   // 引入 std::size_t，表示节点数量与步数。
#include <cstdint>   // 引入 std::uint32_t/uint64_t，保证索引与种子宽度固定。
#include <iostream>  // 引入 std::cout/std::cerr，打印测试结果与失败原因。
#include <string>    // 引入 std::string，拼装失败信息。
#include <vector>    // 引入 std::vector，保存 next 数组与遍历序列。

namespace {  // 匿名命名空间：测试辅助函数只在本文件可见，避免链接冲突。

int failures = 0;  // 失败计数器：每个失败检查加一，最后决定进程退出码。

// check：断言一个条件成立；失败时打印原因并累计失败次数。
void check(bool condition, const std::string& what) {  // condition 为真是通过。
    if (!condition) {  // 条件不成立说明预期没有满足。
        std::cerr << "FAIL: " << what << '\n';  // 把失败原因写到标准错误，CTest 日志可见。
        ++failures;  // 累计失败数。
    }
}

// walk_nodes：测试侧按 next 数组连续走 steps 步，返回节点号序列。
std::vector<std::uint32_t> walk_nodes(const std::vector<std::uint32_t>& next, std::uint32_t start, std::size_t steps) {  // 输入 next、起点与步数。
    std::vector<std::uint32_t> nodes;  // 保存遍历出的节点号。
    nodes.reserve(steps);  // 预分配容量，避免反复扩容。
    std::uint32_t node = start;  // 当前节点从起点开始。
    for (std::size_t step = 0; step < steps; ++step) {  // 连续走 steps 步。
        nodes.push_back(node);  // 记录当前节点。
        node = next[node];  // 沿 next 前进。
    }
    return nodes;  // 返回节点序列。
}

// test_micro_walk：验证任务给出的 4 节点微例。
// 排列 perm=[2,0,3,1] 应构造出 next={3,2,0,1}；从 perm[0]=2 出发，
// 8 步节点序列应为 2,0,3,1,2,0,3,1，对应 node_size=64 的地址序列 128,0,192,64,...。
void test_micro_walk() {  // 无参数，只做断言。
    const std::vector<std::uint32_t> perm = {2, 0, 3, 1};  // 微例给定的节点排列。
    const std::vector<std::uint32_t> next = memwhy::build_chain_from_perm(perm);  // 用公开构造接口生成 next 数组。
    const std::vector<std::uint32_t> expected_next = {3, 2, 0, 1};  // 按 next[perm[i]]=perm[i+1] 推导出的期望值。
    check(next == expected_next, "微例 perm=[2,0,3,1] 应构造出 next={3,2,0,1}");  // next 数组必须逐项一致。
    const std::vector<std::uint32_t> nodes = walk_nodes(next, perm[0], 8);  // 从 perm[0]=2 出发走 8 步。
    const std::vector<std::uint32_t> expected_nodes = {2, 0, 3, 1, 2, 0, 3, 1};  // 微例规定的节点序列。
    check(nodes == expected_nodes, "微例 8 步节点序列应为 2,0,3,1,2,0,3,1");  // 节点序列必须逐项一致。
    std::vector<std::uint64_t> addresses;  // 准备把节点号换算成字节地址。
    for (const std::uint32_t node : nodes) {  // 逐个节点换算。
        addresses.push_back(static_cast<std::uint64_t>(node) * 64);  // 节点 i 的地址 = i × 64（base=0）。
    }
    const std::vector<std::uint64_t> expected_addresses = {128, 0, 192, 64, 128, 0, 192, 64};  // 微例规定的地址序列。
    check(addresses == expected_addresses, "微例地址序列应为 128,0,192,64,128,0,192,64");  // 地址序列必须逐项一致。
}

// test_single_cycle：验证 build_chain 产生的 next 数组是单一环。
// 单环要求：next 的每个值都在 [0,N) 内且每个节点恰好出现一次；
// 从任意节点出发连续走 N 步应不重复地覆盖全部节点，第 N+1 步回到起点。
void test_single_cycle() {  // 无参数，遍历多个规模与种子。
    const std::size_t sizes[] = {1, 2, 3, 4, 17, 64};  // 覆盖最小规模、非 2 的幂与较大规模。
    const std::uint64_t seeds[] = {0, 1, 42, 0x123456789abcdefULL};  // 覆盖多个固定种子。
    for (const std::size_t size : sizes) {  // 对每个节点数。
        for (const std::uint64_t seed : seeds) {  // 对每个种子。
            const std::vector<std::uint32_t> next = memwhy::build_chain(seed, size);  // 构造 next 数组。
            check(next.size() == size, "build_chain 返回的数组长度应等于节点数");  // 长度必须正确。
            std::vector<int> seen(size, 0);  // 记录每个节点在 next 值中出现的次数。
            for (const std::uint32_t value : next) {  // 遍历 next 的每个值。
                check(value < size, "next 的每个值都必须是合法节点号");  // 越界值会破坏链。
                if (value < size) {  // 只有合法值才计数，避免越界访问。
                    ++seen[value];  // 统计该节点被指向的次数。
                }
            }
            bool permutation = true;  // 先假设每个节点恰好被指向一次。
            for (const int count : seen) {  // 检查每个节点的入度。
                if (count != 1) {  // 入度不是 1 说明不是排列。
                    permutation = false;  // 记为失败。
                }
            }
            check(permutation, "next 必须是 0..N-1 的排列（每个节点恰好一个前驱）");  // 排列性质等价于单环的前提。
            const std::vector<std::uint32_t> walk = walk_nodes(next, 0, size + 1);  // 从节点 0 出发走 N+1 步。
            std::vector<int> visited(size, 0);  // 记录前 N 步经过的节点。
            for (std::size_t step = 0; step < size; ++step) {  // 只统计前 N 步。
                if (walk[step] < size) {  // 防御性检查节点号合法。
                    ++visited[walk[step]];  // 标记该节点被访问一次。
                }
            }
            bool covered_once = true;  // 先假设全部节点恰好访问一次。
            for (const int count : visited) {  // 检查每个节点的访问次数。
                if (count != 1) {  // 不是 1 说明有短环或漏节点。
                    covered_once = false;  // 记为失败。
                }
            }
            check(covered_once, "从任意节点走 N 步应恰好覆盖全部节点一次");  // 单环性质的核心断言。
            check(walk[size] == walk[0], "走满 N 步后第 N+1 步应回到起点");  // 闭环必须回到起点。
        }
    }
}

// test_deterministic：同种子两次构造必须得到完全相同的 next 数组。
void test_deterministic() {  // 无参数。
    const std::vector<std::uint32_t> first = memwhy::build_chain(42, 64);  // 第一次构造。
    const std::vector<std::uint32_t> second = memwhy::build_chain(42, 64);  // 同种子第二次构造。
    check(first == second, "同种子的 build_chain 结果必须逐项一致");  // 固定种子纪律要求完全可复现。
    const std::vector<std::uint32_t> other = memwhy::build_chain(43, 64);  // 换一个种子构造。
    check(first != other, "不同种子应得到不同的链排列");  // 64 节点下碰撞概率可以忽略，作为 RNG 生效的冒烟检查。
}

// test_invalid_permutation：非法排列必须报错，不能产出一条坏链。
void test_invalid_permutation() {  // 无参数。
    try {  // 重复节点应被拒绝。
        const std::vector<std::uint32_t> next = memwhy::build_chain_from_perm({0, 0, 1, 2});  // 0 出现两次、3 缺失。
        (void)next;  // 显式忽略变量，避免未使用警告。
        check(false, "重复节点排列应抛出异常");  // 没有抛错说明校验缺失。
    } catch (const std::invalid_argument&) {  // 预期抛出 invalid_argument。
        check(true, "重复节点排列应抛出异常");  // 正确路径。
    }
    try {  // 越界节点应被拒绝。
        const std::vector<std::uint32_t> next = memwhy::build_chain_from_perm({0, 1, 4, 2});  // 4 超出 0..3。
        (void)next;  // 显式忽略变量。
        check(false, "越界节点排列应抛出异常");  // 没有抛错说明校验缺失。
    } catch (const std::invalid_argument&) {  // 预期抛出 invalid_argument。
        check(true, "越界节点排列应抛出异常");  // 正确路径。
    }
}

}  // 匿名命名空间结束。

int main() {  // 测试程序入口。
    test_micro_walk();  // 执行 4 节点微例。
    test_single_cycle();  // 执行单环性质测试。
    test_deterministic();  // 执行同种子复现测试。
    test_invalid_permutation();  // 执行非法排列测试。
    if (failures == 0) {  // 所有检查通过。
        std::cout << "chain_tests: all checks passed\n";  // 打印成功信息。
        return 0;  // 退出码 0 表示成功。
    }
    std::cerr << "chain_tests: " << failures << " check(s) failed\n";  // 打印失败总数。
    return 1;  // 退出码 1 让 CTest 标记失败。
}
