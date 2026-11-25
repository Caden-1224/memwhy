// src/sha256.hpp — SHA-256 摘要的公开接口。
// Bundle 用它对每个成员文件计算 64 个十六进制字符的摘要，写进 manifest 供回放前校验。
#pragma once  // 防止同一编译单元重复包含本头文件，避免函数声明重复。

#include <cstddef>  // 引入 std::size_t，表示输入字节数。
#include <cstdint>  // 引入 std::uint8_t，表示单个字节。
#include <string>   // 引入 std::string，返回十六进制摘要文本。

namespace memwhy {  // 项目命名空间，避免与第三方库符号冲突。

// sha256_hex：计算一段内存字节的 SHA-256，返回 64 个小写十六进制字符。
std::string sha256_hex(const std::uint8_t* data, std::size_t size);

// sha256_file_hex：流式计算一个文件的 SHA-256，返回 64 个小写十六进制字符。
std::string sha256_file_hex(const std::string& path);

}  // namespace memwhy