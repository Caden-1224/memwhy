// src/main.cpp — memwhy 命令行入口。
// 当前阶段只有 --version 一个参数，作用是让构建与 CI 能验证可执行文件确实可运行。
#include <cstdio>   // 提供 std::printf/std::fprintf，向标准输出或标准错误打印文本。
#include <cstring>  // 提供 std::strcmp，比较命令行参数字符串。

int main(int argc, char** argv) {  // main 是程序入口；argc 是参数个数，argv 是参数字符串数组。
    if (argc > 1 && std::strcmp(argv[1], "--version") != 0) {  // 带了参数且第一个参数不是 --version 时，视为用法错误。
        std::fprintf(stderr, "usage: memwhy [--version]\n");  // 把用法提示写到标准错误，便于脚本区分正常输出与错误。
        return 2;  // 返回非零退出码 2，表示命令行用法错误。
    }
    std::printf("memwhy %s\n", MEMWHY_VERSION);  // 打印程序名与 CMake 注入的版本号。
    return 0;  // 返回 0 表示执行成功。
}
