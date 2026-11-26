// src/gen.cpp — `memwhy gen` 子命令的参数解析与 --kind 分发。
// 当前先落地 --kind chain；矩阵与 stride 在同一入口后续扩展，
// 因此这里把所有参数先收敛到结构体，再交给具体生成函数。
#include "gen.hpp"  // 引入 ChainGenOptions 与 run_gen 声明。
#include "cli.hpp"  // 引入共享的命令行参数解析工具。

#include <cstdint>    // 引入 std::uint32_t/std::uint64_t，保存解析结果。
#include <cstdio>     // 提供 std::printf，成功时打印生成摘要。
#include <stdexcept>  // 提供 std::runtime_error，报告参数错误。
#include <string>     // 提供 std::string，保存参数文本。

namespace memwhy {  // 与头文件保持同一命名空间。

namespace {  // 匿名命名空间：解析辅助函数只在 gen.cpp 内可见。

using memwhy::cli::next_value;  // 复用共享的“取下一个参数值”函数。
using memwhy::cli::parse_u32;   // 复用共享的 32 位非负整数解析函数。
using memwhy::cli::parse_u64;   // 复用共享的 64 位非负整数解析函数。

// print_gen_usage：打印 gen 子命令支持的参数。
void print_gen_usage() {  // 无参数，只负责输出。
    std::fprintf(stderr, "%s", kGenUsageLine);  // 复用唯一的用法文本，避免与 main 的总用法漂移。
}

}  // 匿名命名空间结束。

int run_gen(int argc, char** argv) {  // 解析并执行 gen 子命令；argv[0]=memwhy、argv[1]=gen。
    std::string kind;  // 保存 --kind 的值。
    ChainGenOptions options;  // 收集 chain 专属参数。
    bool seed_set = false;  // 记录用户是否显式给了 --seed。
    bool working_set_set = false;  // 记录用户是否显式给了 --working-set。
    bool chain_len_set = false;  // 记录用户是否显式给了 --chain-len。
    for (int i = 2; i < argc; ++i) {  // 从 gen 后面的第一个选项开始解析。
        const std::string option = argv[i];  // 当前选项文本。
        if (option == "--kind") {  // 生成器种类。
            kind = next_value(argc, argv, i);  // 读取下一个参数作为种类名。
        } else if (option == "--seed") {  // 随机种子。
            options.seed = parse_u64(next_value(argc, argv, i), "--seed");  // 解析非负整数。
            seed_set = true;  // 标记已显式给出。
        } else if (option == "--out") {  // 输出轨迹路径。
            options.out_path = next_value(argc, argv, i);  // 读取路径字符串。
        } else if (option == "--working-set") {  // 工作集字节数。
            options.working_set = parse_u64(next_value(argc, argv, i), "--working-set");  // 解析非负整数。
            working_set_set = true;  // 标记已显式给出。
        } else if (option == "--chain-len") {  // 链长（记录条数）。
            options.chain_len = parse_u64(next_value(argc, argv, i), "--chain-len");  // 解析非负整数。
            chain_len_set = true;  // 标记已显式给出。
        } else if (option == "--node-size") {  // 节点大小（字节）。
            options.node_size = parse_u64(next_value(argc, argv, i), "--node-size");  // 解析非负整数。
        } else if (option == "--base") {  // 工作集起始地址。
            options.base = parse_u64(next_value(argc, argv, i), "--base");  // 解析非负整数。
        } else if (option == "--core") {  // 记录核心号。
            options.core = parse_u32(next_value(argc, argv, i), "--core");  // 解析 32 位无符号整数。
        } else {  // 其它选项都不认识。
            throw std::runtime_error("unknown gen option: " + option);  // 报告未知选项，避免拼写错误被静默忽略。
        }
    }
    if (kind.empty()) {  // --kind 是必填项。
        print_gen_usage();  // 打印用法帮助定位。
        throw std::runtime_error("gen requires --kind");  // 报告缺少种类。
    }
    if (kind != "chain") {  // 当前只实现 chain，matrix/stride 后续扩展。
        throw std::runtime_error("unsupported gen kind: " + kind + " (supported: chain)");  // 报告尚未支持的种类。
    }
    if (!seed_set) {  // --seed 是必填项。
        throw std::runtime_error("gen chain requires --seed");  // 报告缺少种子。
    }
    if (options.out_path.empty()) {  // --out 是必填项。
        throw std::runtime_error("gen chain requires --out");  // 报告缺少输出路径。
    }
    if (!working_set_set) {  // --working-set 是必填项。
        throw std::runtime_error("gen chain requires --working-set");  // 报告缺少工作集。
    }
    if (!chain_len_set) {  // --chain-len 是必填项。
        throw std::runtime_error("gen chain requires --chain-len");  // 报告缺少链长。
    }
    generate_chain(options);  // 参数齐全后执行生成。
    const std::uint64_t node_count = options.working_set / options.node_size;  // 工作集包含的节点总数。
    const std::uint64_t touched = options.chain_len < node_count ? options.chain_len : node_count;  // 实际触达的不同节点数；超过一圈后不再增加。
    std::printf("generated chain: %llu records, %llu distinct nodes\n", static_cast<unsigned long long>(options.chain_len), static_cast<unsigned long long>(touched));  // 打印记录数与实际触达节点数口径。
    return 0;  // 成功退出码。
}

}  // MemWhy 命名空间结束。