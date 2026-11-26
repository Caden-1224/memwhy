# CMakeLists.txt — MemWhy 的构建入口。
# CMake 用声明式脚本描述“生成哪些目标、每个目标依赖哪些源文件、测试怎么跑”。
cmake_minimum_required(VERSION 3.20)  # 最低 CMake 3.20：ctest --test-dir 需要这个版本。
project(memwhy VERSION 0.1.0 LANGUAGES CXX)  # 声明项目名、版本号与语言；版本号供 MEMWHY_VERSION 使用。

# 统一 C++ 语言标准，避免不同机器默认标准不一致。
set(CMAKE_CXX_STANDARD 17)  # 使用 C++17（std::filesystem、结构化绑定等）。
set(CMAKE_CXX_STANDARD_REQUIRED ON)  # 编译器不支持 C++17 时直接报错，不静默降级。
set(CMAKE_CXX_EXTENSIONS OFF)  # 关闭 GNU 扩展，保证代码可移植。

# 核心静态库：收纳可被 CLI 与测试共同复用的模型代码。
add_library(memwhy_core STATIC
  src/trace.cpp      # 轨迹 v1 读写实现。
  src/chain.cpp      # 依赖指针链构造与遍历组件，生成器与板端工具共用。
  src/gen_chain.cpp  # 依赖指针链轨迹发射。
  src/gen.cpp        # gen 子命令参数解析与种类分发。
  src/sha256.cpp     # SHA-256 摘要实现，Bundle 成员哈希使用。
  src/bundle.cpp     # Bundle 打包、manifest 与完整性校验。
)
target_include_directories(memwhy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)  # 让链接方可以直接 #include "trace.hpp" 等头文件。

# 命令行可执行文件。
add_executable(memwhy src/main.cpp)  # 程序入口。
target_link_libraries(memwhy PRIVATE memwhy_core)  # 链接核心库，子命令可直接调用核心模块。
target_compile_definitions(memwhy PRIVATE MEMWHY_VERSION="${PROJECT_VERSION}")  # 把版本号注入编译宏。

include(CTest)  # 引入 CTest 测试框架。
if(BUILD_TESTING)  # 只有开启测试时才生成测试目标。
  # 冒烟测试：可执行文件能输出版本号。
  add_test(NAME smoke_version COMMAND memwhy --version)  # 注册冒烟测试。
  string(REPLACE "." "\\." MEMWHY_VERSION_REGEX "${PROJECT_VERSION}")  # 把版本号中的点转义为正则字面量点。
  set_tests_properties(smoke_version PROPERTIES PASS_REGULAR_EXPRESSION "^memwhy ${MEMWHY_VERSION_REGEX}")  # 要求输出以版本号开头。

  # 轨迹 v1 格式测试：布局、往返、同种子复现、损坏检出。
  add_executable(trace_tests tests/trace_tests.cpp)  # 编译测试程序。
  target_link_libraries(trace_tests PRIVATE memwhy_core)  # 测试程序链接核心库。
  add_test(NAME trace_tests COMMAND trace_tests)  # 注册进 CTest，CI 自动执行。

  # Bundle 测试：打包、校验、原子重命名与损坏检出；把 memwhy 可执行文件路径传给测试程序，用于 CLI 驱动。
  add_executable(bundle_tests tests/bundle_tests.cpp)  # 编译 Bundle 测试程序。
  target_link_libraries(bundle_tests PRIVATE memwhy_core)  # 测试程序链接核心库，可直接读取 manifest。
  add_test(NAME bundle_tests COMMAND bundle_tests $<TARGET_FILE:memwhy>)  # 注册进 CTest，并把 CLI 路径作为第一个参数。

  # 依赖指针链构造组件测试：微例序列、单环性质与同种子复现。
  add_executable(chain_tests tests/chain_tests.cpp)  # 编译链构造测试程序。
  target_link_libraries(chain_tests PRIVATE memwhy_core)  # 测试程序链接核心库，直接调用 build_chain。
  add_test(NAME chain_tests COMMAND chain_tests)  # 注册进 CTest，CI 自动执行。

  # 合成轨迹生成器 CLI 测试：地址依赖、单环覆盖、同种子复现与非法参数。
  add_executable(gen_tests tests/gen_tests.cpp)  # 编译生成器测试程序。
  target_link_libraries(gen_tests PRIVATE memwhy_core)  # 测试程序链接核心库，直接读回轨迹与重建链。
  add_test(NAME gen_tests COMMAND gen_tests $<TARGET_FILE:memwhy>)  # 注册进 CTest，并把 CLI 路径作为第一个参数。
endif()  # 结束 BUILD_TESTING 条件块。
