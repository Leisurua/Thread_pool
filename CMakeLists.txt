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
# 单配置生成器未显式指定构建类型时默认 Debug。
if(NOT CMAKE_CONFIGURATION_TYPES)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# --- 语言标准 ---------------------------------------------------------------
# 显式固定 C++20 并禁用 GNU 扩展，避免语言模式随编译器默认标准漂移。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THREAD_POOL_WERROR "把编译警告视为错误" ON)
option(THREAD_POOL_BUILD_BENCHMARKS "构建独立 Release 性能基准" OFF)

# --- 库 ---------------------------------------------------------------------
# 生命周期、关闭、enqueue 与 worker 循环编入静态库，公共编译要求传递给使用方。
add_library(thread_pool STATIC src/thread_pool.cpp)
add_library(thread_pool::thread_pool ALIAS thread_pool)

target_include_directories(thread_pool PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(thread_pool PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(thread_pool PUBLIC Threads::Threads)

# 统一配置库及使用方的编译警告。
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
if(THREAD_POOL_BUILD_BENCHMARKS)
    add_executable(thread_pool_benchmark
        benchmarks/thread_pool_benchmark.cpp benchmarks/workloads.cpp)
    target_link_libraries(thread_pool_benchmark PRIVATE thread_pool::thread_pool)
    set_property(TARGET thread_pool_benchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
    target_compile_definitions(thread_pool_benchmark PRIVATE
        THREAD_POOL_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
endif()

if(BUILD_TESTING)
    add_executable(thread_pool_basic_execution_test tests/basic_execution_test.cpp)
    target_link_libraries(thread_pool_basic_execution_test PRIVATE thread_pool::thread_pool)
    foreach(basic_case IN ITEMS workers exactly_once future_results move_only_and_ref exception_recovery)
        add_test(NAME thread_pool_basic_${basic_case}
            COMMAND thread_pool_basic_execution_test ${basic_case})
        set_tests_properties(thread_pool_basic_${basic_case} PROPERTIES
            TIMEOUT 30 LABELS basic_execution)
    endforeach()

    add_executable(thread_pool_shutdown_test tests/shutdown_test.cpp)
    target_link_libraries(thread_pool_shutdown_test PRIVATE thread_pool::thread_pool)
    foreach(shutdown_case IN ITEMS empty_queue drains_queue waits_running rejects_submit repeated_shutdown concurrent_shutdown)
        add_test(NAME thread_pool_shutdown_${shutdown_case}
            COMMAND thread_pool_shutdown_test ${shutdown_case})
        set_tests_properties(thread_pool_shutdown_${shutdown_case} PROPERTIES
            TIMEOUT 30 LABELS graceful_shutdown)
    endforeach()

    add_executable(thread_pool_shutdown_now_test tests/shutdown_now_test.cpp)
    target_link_libraries(thread_pool_shutdown_now_test PRIVATE thread_pool::thread_pool)
    foreach(shutdown_now_case IN ITEMS discards_unclaimed discarded_future claimed_result idle_workers_exit claim_race waits_running)
        add_test(NAME thread_pool_shutdown_now_${shutdown_now_case}
            COMMAND thread_pool_shutdown_now_test ${shutdown_now_case})
        set_tests_properties(thread_pool_shutdown_now_${shutdown_now_case} PROPERTIES
            TIMEOUT 30 LABELS immediate_shutdown)
    endforeach()

    add_executable(thread_pool_close_race_test tests/close_race_test.cpp)
    target_link_libraries(thread_pool_close_race_test PRIVATE thread_pool::thread_pool)
    foreach(close_race_case IN ITEMS first_mode waiters_wait worker_self_close owner_reentry other_pool_worker repeated_close)
        add_test(NAME thread_pool_close_race_${close_race_case}
            COMMAND thread_pool_close_race_test ${close_race_case})
        set_tests_properties(thread_pool_close_race_${close_race_case} PROPERTIES
            TIMEOUT 30 LABELS close_race)
    endforeach()

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
    foreach(lifecycle_case IN ITEMS worker_count zero_worker_count
            partial_construction_failure destruction_drains destruction_after_close
            enqueue_allocation_failure capture_destroyed_outside_lock worker_self_close
            concurrent_close_graceful concurrent_close_immediate discarded_capture_reentry
            shutdown_preparation_failure)
        add_test(NAME thread_pool_lifecycle_${lifecycle_case}
            COMMAND thread_pool_lifecycle_test ${lifecycle_case})
        set_tests_properties(thread_pool_lifecycle_${lifecycle_case} PROPERTIES
            TIMEOUT 30 LABELS lifecycle)
    endforeach()
endif()
