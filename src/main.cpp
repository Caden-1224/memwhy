// src/main.cpp — memwhy 命令行入口。
// 当前支持 --version 与 bundle pack/verify 两组命令；后续子命令沿用同一套参数解析方式扩展。
#include "bundle.hpp"  // 引入 Bundle 打包与校验接口。

#include <cstdint>    // 引入 std::uint64_t，保存解析出的记录数。
#include <cstdio>     // 提供 std::printf/std::fprintf，向标准输出或错误打印文本。
#include <cstring>    // 提供 std::strcmp，比较命令行参数字符串。
#include <exception>  // 提供 std::exception，捕获所有标准异常。
#include <stdexcept>  // 提供 std::runtime_error/std::invalid_argument。
#include <string>     // 提供 std::string，保存参数与错误信息。

namespace {  // 匿名命名空间：命令行辅助函数只在 main.cpp 内可见。

// print_usage：打印当前支持的命令行用法；错误路径统一调用它。
void print_usage() {  // 无参数，只负责输出。
    std::fprintf(stderr,  // 用法信息写到标准错误，方便脚本区分正常输出与错误。
                 "usage: memwhy [--version]\n"  // 版本查询用法。
                 "       memwhy bundle pack --trace PATH --config PATH --out DIR [--env PATH] [--warmup N] [--measure-start N] [--measure-count M]\n"  // 打包用法。
                 "       memwhy bundle verify --bundle DIR\n");  // 校验用法。
}

// next_value：取出当前选项后面紧跟的参数值；缺失时抛出用法错误。
const char* next_value(int argc, char** argv, int& index) {  // index 是当前选项下标，会被前移到值的位置。
    if (index + 1 >= argc) {  // 后面没有参数说明用户漏写值。
        throw std::runtime_error(std::string("missing value for ") + argv[index]);  // 报告缺少哪个选项的值。
    }
    ++index;  // 下标前移到值参数。
    return argv[index];  // 返回值字符串。
}

// parse_u64：把命令行字符串解析为非负 64 位整数；拒绝负号、尾随字符与溢出。
std::uint64_t parse_u64(const char* value, const char* option) {  // value 是待解析文本，option 用于报错定位。
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

// run_bundle_pack：解析并执行 `memwhy bundle pack ...`。
int run_bundle_pack(int argc, char** argv) {  // argv[0]=memwhy、argv[1]=bundle、argv[2]=pack。
    memwhy::BundlePackOptions options;  // 收集所有打包参数。
    for (int i = 3; i < argc; ++i) {  // 从第一个选项开始逐个解析。
        const std::string option = argv[i];  // 当前选项文本。
        if (option == "--trace") {  // 轨迹输入路径。
            options.trace_path = next_value(argc, argv, i);  // 读取下一个参数作为路径。
        } else if (option == "--config") {  // 配置输入路径。
            options.config_path = next_value(argc, argv, i);  // 读取配置路径。
        } else if (option == "--out") {  // 输出 Bundle 目录。
            options.out_dir = next_value(argc, argv, i);  // 读取输出路径。
        } else if (option == "--env") {  // 可选环境快照路径。
            options.env_path = next_value(argc, argv, i);  // 读取环境快照路径。
        } else if (option == "--warmup") {  // 预热记录数。
            options.warmup = parse_u64(next_value(argc, argv, i), "--warmup");  // 解析非负整数。
        } else if (option == "--measure-start") {  // 测量窗口起始下标。
            options.measure_start = parse_u64(next_value(argc, argv, i), "--measure-start");  // 解析非负整数。
        } else if (option == "--measure-count") {  // 测量窗口记录数。
            options.measure_count = parse_u64(next_value(argc, argv, i), "--measure-count");  // 解析非负整数。
            options.measure_count_set = true;  // 标记用户显式给出了窗口长度。
        } else {  // 其它选项都不认识。
            throw std::runtime_error("unknown bundle pack option: " + option);  // 报告未知选项，避免拼写错误被静默忽略。
        }
    }
    if (options.trace_path.empty() || options.config_path.empty() || options.out_dir.empty()) {  // 三个必填参数必须齐全。
        throw std::runtime_error("bundle pack requires --trace, --config and --out");  // 报告缺少必填参数。
    }
    memwhy::pack_bundle(options);  // 执行打包。
    std::printf("packed %s\n", options.out_dir.c_str());  // 成功后打印目标目录，便于脚本确认。
    return 0;  // 成功退出码。
}

// run_bundle_verify：解析并执行 `memwhy bundle verify --bundle DIR`。
int run_bundle_verify(int argc, char** argv) {  // argv[0]=memwhy、argv[1]=bundle、argv[2]=verify。
    std::string bundle_dir;  // 待校验的 Bundle 目录。
    for (int i = 3; i < argc; ++i) {  // 从第一个选项开始解析。
        const std::string option = argv[i];  // 当前选项文本。
        if (option == "--bundle") {  // Bundle 目录选项。
            bundle_dir = next_value(argc, argv, i);  // 读取目录路径。
        } else {  // 其它选项不认识。
            throw std::runtime_error("unknown bundle verify option: " + option);  // 报告未知选项。
        }
    }
    if (bundle_dir.empty()) {  // 必须给出 Bundle 目录。
        throw std::runtime_error("bundle verify requires --bundle");  // 报告缺少必填参数。
    }
    const memwhy::BundleManifest manifest = memwhy::verify_bundle(bundle_dir);  // 执行校验并取得 manifest。
    std::printf("bundle ok: %zu members\n", manifest.members.size());  // 打印成员数，作为成功信号。
    return 0;  // 成功退出码。
}

// run_bundle：分发 bundle 子命令。
int run_bundle(int argc, char** argv) {  // argc/argv 来自 main。
    if (argc < 3) {  // 只有 `memwhy bundle` 没有子命令。
        print_usage();  // 打印用法。
        return 2;  // 用法错误退出码。
    }
    const std::string command = argv[2];  // 取出子命令名。
    if (command == "pack") {  // 打包子命令。
        return run_bundle_pack(argc, argv);  // 交给打包解析器。
    }
    if (command == "verify") {  // 校验子命令。
        return run_bundle_verify(argc, argv);  // 交给校验解析器。
    }
    throw std::runtime_error("unknown bundle command: " + command);  // 未知子命令直接报错。
}

}  // 匿名命名空间结束。

int main(int argc, char** argv) {  // main 是程序入口；argc 是参数个数，argv 是参数字符串数组。
    try {  // 统一捕获参数错误与运行时错误，保证进程返回非零而不是崩溃。
        if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {  // 查询版本。
            std::printf("memwhy %s\n", MEMWHY_VERSION);  // 打印程序名与 CMake 注入的版本号。
            return 0;  // 成功退出码。
        }
        if (argc >= 2 && std::strcmp(argv[1], "bundle") == 0) {  // bundle 子命令族。
            return run_bundle(argc, argv);  // 分发到具体子命令。
        }
        print_usage();  // 其它参数组合都视为用法错误。
        return 2;  // 用法错误退出码。
    } catch (const std::exception& error) {  // 捕获所有标准异常。
        std::fprintf(stderr, "memwhy: %s\n", error.what());  // 把具体错误原因写到标准错误。
        return 1;  // 运行时错误退出码。
    }
}