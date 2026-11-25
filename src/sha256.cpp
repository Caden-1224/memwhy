// src/sha256.cpp — SHA-256 摘要算法的自包含实现。
// 选择自己实现而不是引入 OpenSSL：Bundle 只需要一个稳定的哈希函数，
// 自带实现让构建没有外部依赖，也避免不同发行版 OpenSSL 版本差异。
#include "sha256.hpp"  // 引入本模块的公开声明，保证定义与头文件一致。

#include <algorithm>  // 提供 std::min，计算本次能拷入缓冲区的字节数。
#include <array>      // 提供 std::array，保存固定长度的状态与摘要。
#include <cstring>    // 提供 std::memcpy/std::memset，处理消息缓冲区。
#include <fstream>    // 提供 std::ifstream，流式读取文件。
#include <stdexcept>  // 提供 std::runtime_error，报告文件打开或读取失败。
#include <string>     // 提供 std::string，拼装错误信息与十六进制结果。

namespace memwhy {  // 与头文件保持同一个命名空间。
namespace {  // 匿名命名空间：常量与内部类只在 sha256.cpp 内可见。

// SHA-256 的 64 个轮常量，来自 FIPS 180-4 标准；每个值都是前 64 个质数立方根小数部分的前 32 位。
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,  // 第 1–4 轮常量：标准定义，不能改。
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,  // 第 5–8 轮常量：标准定义，不能改。
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,  // 第 9–12 轮常量：标准定义，不能改。
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,  // 第 13–16 轮常量：标准定义，不能改。
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,  // 第 17–20 轮常量：标准定义，不能改。
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,  // 第 21–24 轮常量：标准定义，不能改。
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,  // 第 25–28 轮常量：标准定义，不能改。
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,  // 第 29–32 轮常量：标准定义，不能改。
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,  // 第 33–36 轮常量：标准定义，不能改。
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,  // 第 37–40 轮常量：标准定义，不能改。
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,  // 第 41–44 轮常量：标准定义，不能改。
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,  // 第 45–48 轮常量：标准定义，不能改。
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,  // 第 49–52 轮常量：标准定义，不能改。
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,  // 第 53–56 轮常量：标准定义，不能改。
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,  // 第 57–60 轮常量：标准定义，不能改。
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,  // 第 61–64 轮常量：标准定义，不能改。
};

// rotr：32 位无符号整数循环右移；SHA-256 的扩散步骤大量使用它。
std::uint32_t rotr(std::uint32_t value, unsigned bits) {  // 参数 bits 必须落在 1..31，本文件所有调用都满足。
    return (value >> bits) | (value << (32u - bits));  // 右移丢掉的低位通过左移补回高位，拼成循环移位。
}

// choose：按位选择函数 Ch(x,y,z) = (x & y) ^ (~x & z)，逐位从 y 或 z 中选择。
std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) {  // 三个 32 位输入。
    return (x & y) ^ (~x & z);  // x 为 1 的位取 y，x 为 0 的位取 z。
}

// majority：按位多数函数 Maj(x,y,z) = (x & y) ^ (x & z) ^ (y & z)。
std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) {  // 三个 32 位输入。
    return (x & y) ^ (x & z) ^ (y & z);  // 每一位等于三个输入中至少两个为 1 的结果。
}

// Sha256：增量式计算 SHA-256 的内部类；调用方只需 update() 再 finish()。
class Sha256 {
public:
    Sha256() : buffer_size_(0), total_bytes_(0) {  // 构造时缓冲区为空、总字节数为 0。
        state_[0] = 0x6a09e667u;  // 第 1 个初始状态字：sqrt(2) 小数部分前 32 位。
        state_[1] = 0xbb67ae85u;  // 第 2 个初始状态字：sqrt(3) 小数部分前 32 位。
        state_[2] = 0x3c6ef372u;  // 第 3 个初始状态字：sqrt(5) 小数部分前 32 位。
        state_[3] = 0xa54ff53au;  // 第 4 个初始状态字：sqrt(7) 小数部分前 32 位。
        state_[4] = 0x510e527fu;  // 第 5 个初始状态字：sqrt(11) 小数部分前 32 位。
        state_[5] = 0x9b05688cu;  // 第 6 个初始状态字：sqrt(13) 小数部分前 32 位。
        state_[6] = 0x1f83d9abu;  // 第 7 个初始状态字：sqrt(17) 小数部分前 32 位。
        state_[7] = 0x5be0cd19u;  // 第 8 个初始状态字：sqrt(19) 小数部分前 32 位。
    }

    // update：追加一段字节；内部自动按 64 字节分组处理，剩余不足一组留在缓冲区。
    void update(const std::uint8_t* data, std::size_t size) {  // data 指向输入字节，size 是字节数。
        total_bytes_ += static_cast<std::uint64_t>(size);  // 累计总长度，finish() 用它写 64 位比特长度。
        while (size > 0) {  // 只要还有输入字节就继续填充缓冲区。
            const std::size_t room = 64 - buffer_size_;  // 当前缓冲区还能放多少字节。
            const std::size_t take = std::min(size, room);  // 本次最多只能填满一个 64 字节分组。
            std::memcpy(buffer_.data() + buffer_size_, data, take);  // 把 take 个字节追加到缓冲区尾部。
            buffer_size_ += take;  // 缓冲区有效字节数增加。
            data += take;  // 输入指针前移。
            size -= take;  // 剩余输入减少。
            if (buffer_size_ == 64) {  // 缓冲区满 64 字节，正好是一组。
                transform(buffer_.data());  // 压缩这一组，更新 8 个状态字。
                buffer_size_ = 0;  // 清空缓冲区，准备接收下一组。
            }
        }
    }

    // finish：按 SHA-256 规则补位并输出 32 字节摘要；调用后对象不应继续使用。
    std::array<std::uint8_t, 32> finish() {  // 返回 32 字节（256 位）摘要。
        const std::uint64_t bit_length = total_bytes_ * 8u;  // SHA-256 在末尾写的是原始消息的比特长度。
        buffer_[buffer_size_++] = 0x80u;  // 先追加一个 1 比特（字节形式 0x80），这是补位起点。
        if (buffer_size_ > 56) {  // 如果补 0x80 后剩余空间不足 8 字节长度字段。
            std::memset(buffer_.data() + buffer_size_, 0, 64 - buffer_size_);  // 本组剩余字节全部补 0。
            transform(buffer_.data());  // 先压缩这一组。
            buffer_size_ = 0;  // 新的一组从空开始。
        }
        std::memset(buffer_.data() + buffer_size_, 0, 56 - buffer_size_);  // 补 0 直到本组前 56 字节结束。
        for (int i = 0; i < 8; ++i) {  // 把 64 位比特长度按大端写入最后 8 字节。
            buffer_[56 + i] = static_cast<std::uint8_t>((bit_length >> (56 - 8 * i)) & 0xffu);  // 第 i 个字节取从高位数第 i 个 8 位。
        }
        transform(buffer_.data());  // 压缩最后这一组。
        std::array<std::uint8_t, 32> digest{};  // 准备 32 字节输出缓冲区。
        for (int i = 0; i < 8; ++i) {  // 把 8 个 32 位状态字按大端写成 32 字节。
            digest[4 * i + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);  // 状态字的最高 8 位放在前面。
            digest[4 * i + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);  // 次高 8 位。
            digest[4 * i + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);  // 再次高 8 位。
            digest[4 * i + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);  // 最低 8 位。
        }
        return digest;  // 返回最终摘要。
    }

private:
    // transform：对单个 64 字节分组执行 64 轮压缩。
    void transform(const std::uint8_t* block) {  // block 必须指向 64 个字节。
        std::array<std::uint32_t, 64> w{};  // 消息调度数组，前 16 项来自分组，其余由公式扩展。
        for (int i = 0; i < 16; ++i) {  // 先把 64 字节按大端拆成 16 个 32 位字。
            w[i] = (static_cast<std::uint32_t>(block[4 * i + 0]) << 24) |  // 第 1 个字节是最高 8 位。
                   (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |  // 第 2 个字节是次高 8 位。
                   (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) |  // 第 3 个字节。
                   (static_cast<std::uint32_t>(block[4 * i + 3]));  // 第 4 个字节是最低 8 位。
        }
        for (int i = 16; i < 64; ++i) {  // 扩展到 64 个字。
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);  // 小 σ0 函数。
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);  // 小 σ1 函数。
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;  // 当前字由前 16 字、前 7 字与两个 σ 结果相加。
        }
        std::uint32_t a = state_[0];  // 工作变量 a 从状态字第 1 个开始。
        std::uint32_t b = state_[1];  // 工作变量 b。
        std::uint32_t c = state_[2];  // 工作变量 c。
        std::uint32_t d = state_[3];  // 工作变量 d。
        std::uint32_t e = state_[4];  // 工作变量 e。
        std::uint32_t f = state_[5];  // 工作变量 f。
        std::uint32_t g = state_[6];  // 工作变量 g。
        std::uint32_t h = state_[7];  // 工作变量 h。
        for (int i = 0; i < 64; ++i) {  // 执行 64 轮。
            const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);  // 大 Σ1(e)。
            const std::uint32_t ch = choose(e, f, g);  // Ch(e,f,g)。
            const std::uint32_t temp1 = h + big_s1 + ch + kRoundConstants[i] + w[i];  // 轮函数临时值 1。
            const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);  // 大 Σ0(a)。
            const std::uint32_t maj = majority(a, b, c);  // Maj(a,b,c)。
            const std::uint32_t temp2 = big_s0 + maj;  // 轮函数临时值 2。
            h = g;  // 状态寄存器整体右移：h 取 g。
            g = f;  // g 取 f。
            f = e;  // f 取 e。
            e = d + temp1;  // e 取 d 加临时值 1。
            d = c;  // d 取 c。
            c = b;  // c 取 b。
            b = a;  // b 取 a。
            a = temp1 + temp2;  // a 取两个临时值之和。
        }
        state_[0] += a;  // 把本轮结果累加回初始状态字 1。
        state_[1] += b;  // 累加回状态字 2。
        state_[2] += c;  // 累加回状态字 3。
        state_[3] += d;  // 累加回状态字 4。
        state_[4] += e;  // 累加回状态字 5。
        state_[5] += f;  // 累加回状态字 6。
        state_[6] += g;  // 累加回状态字 7。
        state_[7] += h;  // 累加回状态字 8。
    }

    std::array<std::uint32_t, 8> state_{};  // 8 个 32 位状态字，保存中间摘要状态。
    std::array<std::uint8_t, 64> buffer_{};  // 64 字节分组缓冲区，存放尚未凑满一组的数据。
    std::size_t buffer_size_;  // 缓冲区当前有效字节数（0..63）。
    std::uint64_t total_bytes_;  // 已输入的总字节数，finish() 用它计算比特长度。
};

// to_hex：把 32 字节摘要转换成 64 个小写十六进制字符。
std::string to_hex(const std::array<std::uint8_t, 32>& digest) {  // 输入固定 32 字节摘要。
    static const char kHex[] = "0123456789abcdef";  // 十六进制数字表，索引 0..15 对应字符。
    std::string out;  // 结果字符串。
    out.reserve(64);  // 预留 64 个字符，避免反复扩容。
    for (const std::uint8_t byte : digest) {  // 逐字节转换。
        out.push_back(kHex[byte >> 4]);  // 高 4 位对应的十六进制字符。
        out.push_back(kHex[byte & 0x0fu]);  // 低 4 位对应的十六进制字符。
    }
    return out;  // 返回 64 字符的摘要文本。
}

}  // 匿名命名空间结束。

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {  // 公开接口：计算内存字节的摘要。
    Sha256 hasher;  // 创建增量哈希对象。
    hasher.update(data, size);  // 一次性喂入全部字节。
    return to_hex(hasher.finish());  // 补位、压缩并转成十六进制文本。
}

std::string sha256_file_hex(const std::string& path) {  // 公开接口：流式计算文件摘要。
    std::ifstream input(path, std::ios::binary);  // 以二进制模式打开文件，避免换行转换影响哈希。
    if (!input) {  // 打开失败说明路径不存在或没有读权限。
        throw std::runtime_error("sha256: cannot open '" + path + "'");  // 抛出带路径的错误，调用方负责展示。
    }
    Sha256 hasher;  // 创建增量哈希对象。
    std::array<char, 4096> buffer{};  // 4 KiB 读缓冲，兼顾系统调用次数与内存占用。
    while (input) {  // 只要流状态正常就继续读。
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));  // 尝试读满 4 KiB。
        const std::streamsize got = input.gcount();  // gcount() 返回本次实际读到的字节数。
        if (got > 0) {  // 有数据才更新哈希。
            hasher.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(got));  // 把读到的字节喂给哈希对象。
        }
    }
    if (!input.eof()) {  // 循环结束若不是因为读到文件尾，说明发生了真实读取错误。
        throw std::runtime_error("sha256: read failed for '" + path + "'");  // 报告读取失败，绝不返回半截文件的摘要。
    }
    return to_hex(hasher.finish());  // 返回完整文件的十六进制摘要。
}

}  // namespace memwhy