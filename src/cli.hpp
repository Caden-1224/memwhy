// src/cli.hpp — 命令行解析的共享小工具。
// main.cpp 的 bundle 子命令与 gen.cpp 的 gen 子命令都需要“取下一个参数值”和“解析非负整数”，
// 放在同一个头文件里避免两份实现随参数规则变化而漂移。
#pragma once  // 防止同一编译单元重复包含本头文件。

#include <cstddef>   // 提供 std::size_t，记录 stoull 消费的字符数。
#include <cstdint>   // 提供 std::uint32_t/std::uint64_t，保证解析结果宽度固定。
#include <exception> // 提供 std::exception，捕获 stoull 抛出的标准异常。
#include <limits>    // 提供 std::numeric_limits，检查 32 位参数是否越界。
#include <stdexcept> // 提供 std::runtime_error/std::invalid_argument。
#include <string>    // 提供 std::string，保存参数文本与错误信息。

namespace memwhy {  // 项目命名空间。
namespace cli {     // 命令行辅助函数子命名空间，避免与业务函数重名。

// next_value：取出当前选项后面紧跟的参数值；缺失时抛出用法错误。
inline const char* next_value(int argc, char** argv, int& index) {  // index 是当前选项下标，会被前移到值的位置。
    if (index + 1 >= argc) {  // 后面没有参数说明用户漏写值。
        throw std::runtime_error(std::string("missing value for ") + argv[index]);  // 报告缺少哪个选项的值。
    }
    ++index;  // 下标前移到值参数。
    return argv[index];  // 返回值字符串。
}

// parse_u64：把命令行字符串解析为非负 64 位整数；拒绝负号、尾随字符与溢出。
inline std::uint64_t parse_u64(const char* value, const char* option) {  // value 是待解析文本，option 用于报错定位。
    const std::string text(value);  // 复制成 std::string，便于检查长度。
    if (text.empty() || text.front() < '0' || text.front() > '9') {  // 只接受数字开头，显式拒绝负号、正号与空串。
        throw std::runtime_error(std::string("invalid integer for ") + option + ": " + text);  // 报告非法整数。
    }
    try {  // std::stoull 在非法输入或溢出时抛异常。
        std::size_t used = 0;  // 记录 stoull 实际消费的字符数。
        const unsigned long long parsed = std::stoull(text, &used, 10);  // 按十进制解析无符号长整数。
        if (used != text.size()) {  // 还有剩余字符说明不是纯数字。
            throw std::invalid_argument("trailing characters");  // 主动抛错进入统一处理。
        }
        return static_cast<std::uint64_t>(parsed);  // 转成固定宽度 64 位整数。
    } catch (const std::exception&) {  // 捕获解析失败与手动抛出的异常。
        throw std::runtime_error(std::string("invalid integer for ") + option + ": " + text);  // 统一报告哪个选项的哪个值非法。
    }
}

// parse_u32：解析 32 位无符号整数，用于 core 这类小范围参数。
inline std::uint32_t parse_u32(const char* value, const char* option) {  // value 是待解析文本。
    const std::uint64_t parsed = parse_u64(value, option);  // 先按 64 位解析，避免直接截断。
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {  // 超出 32 位上限就报错。
        throw std::runtime_error(std::string("value out of range for ") + option + ": " + std::string(value));  // 报告哪个选项越界。
    }
    return static_cast<std::uint32_t>(parsed);  // 安全转成 32 位。
}

}  // cli 子命名空间结束。
}  // MemWhy 命名空间结束。
