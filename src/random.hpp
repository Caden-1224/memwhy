// src/random.hpp — 固定种子随机采样工具。
// 跨平台“同种子逐字节一致”的关键不是随机数本身，而是随机数的产生方式必须被标准锁定。
// C++ 的 std::mt19937_64 算法由标准规定，输出序列可跨标准库复现；
// 而 std::uniform_int_distribution 的内部算法由实现自由决定，同一 seed 在 libstdc++ 与 libc++ 上可能不同，
// 所以这里手写拒绝采样（rejection sampling）把 64 位随机数映射到指定范围。
#pragma once  // 防止同一编译单元重复包含本头文件；跨编译单元的 inline 定义由 C++ 标准 ODR 规则合并。

#include <cstdint>  // 引入 std::uint64_t，保证随机数与上界都是 64 位无符号整数。
#include <random>   // 引入 std::mt19937_64，标准化的 64 位梅森旋转引擎。

namespace memwhy {  // 与项目其他模块共用命名空间。

// uniform_below 返回 [0, bound) 内的均匀整数；bound 必须大于 0。
inline std::uint64_t uniform_below(std::mt19937_64& rng, std::uint64_t bound) {
    // threshold = 2^64 % bound：小于 threshold 的样本会让取模结果偏斜，必须丢弃重抽。
    // 这里用无符号减法 (0ULL - bound) 得到 2^64 - bound，再对 bound 取模，等价于 2^64 % bound。
    const std::uint64_t threshold = (0ULL - bound) % bound;
    std::uint64_t value = 0;  // 保存本次抽取的 64 位随机数。
    do {  // 至少抽取一次，直到样本落到无偏区间。
        value = rng();  // 从引擎取出下一个确定性的 64 位输出。
    } while (value < threshold);  // 落在偏置区间的样本直接丢弃，重新抽取。
    return value % bound;  // 此时每个余数出现的概率完全相同，取模不再引入偏置。
}

}  // namespace memwhy
