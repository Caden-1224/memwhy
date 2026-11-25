cmake_minimum_required(VERSION 3.20)
project(memwhy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(memwhy src/main.cpp)
target_compile_definitions(memwhy PRIVATE MEMWHY_VERSION="${PROJECT_VERSION}")

include(CTest)
if(BUILD_TESTING)
  add_test(NAME smoke_version COMMAND memwhy --version)
  string(REPLACE "." "\\." MEMWHY_VERSION_REGEX "${PROJECT_VERSION}")
  set_tests_properties(smoke_version PROPERTIES PASS_REGULAR_EXPRESSION "^memwhy ${MEMWHY_VERSION_REGEX}")
endif()
