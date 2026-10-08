cmake_minimum_required(VERSION 3.25)

project(thread_pool
    VERSION 1.0.0
    DESCRIPTION "C++20 固定 worker 数量 FIFO 线程池"
    LANGUAGES CXX)

# 本机预设明确指定工具链版本，避免 PATH 或工具升级造成环境漂移。
if(DEFINED THREAD_POOL_EXPECTED_GCC_VERSION)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
       NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL THREAD_POOL_EXPECTED_GCC_VERSION)
        message(FATAL_ERROR
            "本机预设要求 GNU GCC ${THREAD_POOL_EXPECTED_GCC_VERSION}，"
            "当前为 ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
endif()

# --- 构建类型 ---------------------------------------------------------------
# 本机使用 Ninja(单配置生成器)：未显式指定时默认 Debug，与 README 声明的
# 「CPP20 Windows / MSYS2 ucrt64 GNU GCC 16.2.0、CMake + Ninja、Debug」一致。
if(NOT CMAKE_CONFIGURATION_TYPES)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# --- 语言标准 ---------------------------------------------------------------
# GCC 16.2.0 的默认标准已是 C++20，CMake 因而不会补 -std= 参数；只写
# target_compile_features(cxx_std_20) 的话，合规性其实由编译器默认值兜底，
# 将来编译器默认升到 C++23（或换用更旧的 GCC）语言模式就会漂移。
# 这里显式钉死：EXTENSIONS OFF 使实际参数为 -std=c++20（而非 -std=gnu++20），
# 与 docs/IMPLEMENTATION.md 记录的手工命令一致。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THREAD_POOL_WERROR "把编译警告视为错误" ON)

# --- 库 ---------------------------------------------------------------------
# 池构造/析构及 worker 循环在源文件中编译，其余实现与公共编译要求由静态库传递。
add_library(thread_pool STATIC src/thread_pool.cpp)
add_library(thread_pool::thread_pool ALIAS thread_pool)

target_include_directories(thread_pool PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(thread_pool PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(thread_pool PUBLIC Threads::Threads)

# 警告集与 docs/IMPLEMENTATION.md 记录的手工 g++ 命令保持一致。
if(MSVC)
    set(THREAD_POOL_WARNINGS /W4 /permissive-)
else()
    set(THREAD_POOL_WARNINGS -Wall -Wextra -Wpedantic)
endif()
target_compile_options(thread_pool PUBLIC
    "$<$<COMPILE_LANGUAGE:CXX>:${THREAD_POOL_WARNINGS}>")

if(THREAD_POOL_WERROR)
    target_compile_options(thread_pool PUBLIC
        "$<$<COMPILE_LANGUAGE:CXX>:$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>>")
endif()

# 验证头文件能独立编译且可重复包含，不要求链接完整线程池实现。
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/thread_pool_header_check.cpp"
    CONTENT "#include \"thread_pool.hpp\"\n#include \"thread_pool.hpp\"\n")
add_library(thread_pool_header_check OBJECT
    "${CMAKE_CURRENT_BINARY_DIR}/thread_pool_header_check.cpp")
target_link_libraries(thread_pool_header_check PRIVATE thread_pool::thread_pool)

include(CTest)
if(BUILD_TESTING)
    add_executable(thread_pool_submit_test tests/submit_test.cpp)
    target_link_libraries(thread_pool_submit_test PRIVATE thread_pool::thread_pool)
    add_test(NAME thread_pool_submit COMMAND thread_pool_submit_test)
    set_tests_properties(thread_pool_submit PROPERTIES TIMEOUT 30)

    add_executable(thread_pool_worker_test tests/worker_test.cpp)
    target_link_libraries(thread_pool_worker_test PRIVATE thread_pool::thread_pool)
    add_test(NAME thread_pool_worker COMMAND thread_pool_worker_test)
    set_tests_properties(thread_pool_worker PROPERTIES TIMEOUT 30)

    add_executable(thread_pool_lifecycle_test tests/lifecycle_test.cpp)
    target_link_libraries(thread_pool_lifecycle_test PRIVATE thread_pool::thread_pool)
    add_test(NAME thread_pool_lifecycle COMMAND thread_pool_lifecycle_test)
    set_tests_properties(thread_pool_lifecycle PROPERTIES TIMEOUT 30)
endif()
