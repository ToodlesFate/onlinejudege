cmake_minimum_required(VERSION 3.20)
project(judge-tool CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ----------------------------------------------------------------------------
#  静态构建选项 —— 用于 judge 镜像内嵌（SPEC §6.1 要求 5MB）
#  打开方式：cmake -DJUDGE_STATIC=ON ..
# ----------------------------------------------------------------------------
option(JUDGE_STATIC "Build judge with static linkage (no libstdc++/libgcc needed)" OFF)

# 关闭所有子项目的 install / 测试 / 例子
set(FETCHCONTENT_QUIET OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
include(FetchContent)

# ----------------------------------------------------------------------------
#  nlohmann/json —— header-only
#  默认通过 FetchContent 拉；可在容器构建时设 JUDGE_NO_FETCHCONTENT=ON
#  改用系统安装的 libnlohmann-json3-dev（避免容器内 git fetch 超时）
# ----------------------------------------------------------------------------
option(JUDGE_NO_FETCHCONTENT "Use system-installed nlohmann/json (skip FetchContent)" OFF)
if(JUDGE_NO_FETCHCONTENT)
    find_package(nlohmann_json 3.10 REQUIRED)
else()
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ----------------------------------------------------------------------------
#  googletest —— 仅 tests target 用
# ----------------------------------------------------------------------------
option(JUDGE_TOOL_BUILD_TESTS "Build judge-tool unit/integration tests" ON)
if(JUDGE_TOOL_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# ----------------------------------------------------------------------------
#  核心库：judge_core —— 供 main 链接 + 测试链接
# ----------------------------------------------------------------------------
add_library(judge_core STATIC
    src/meta.cpp
    src/compile.cpp
    src/runner.cpp
    src/monitor.cpp
    src/compare.cpp
    src/result.cpp
    src/status.cpp
)
target_include_directories(judge_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(judge_core PUBLIC nlohmann_json::nlohmann_json)
if(CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE Release)
endif()

# ----------------------------------------------------------------------------
#  judge 二进制 —— 容器内 entrypoint 调用
# ----------------------------------------------------------------------------
add_executable(judge src/main.cpp)
target_link_libraries(judge PRIVATE judge_core)
if(JUDGE_STATIC)
    target_link_options(judge PRIVATE -static-libstdc++ -static-libgcc)
    target_compile_options(judge PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(judge PRIVATE -Wl,--gc-sections -Wl,-s)
endif()

# ----------------------------------------------------------------------------
#  tests
# ----------------------------------------------------------------------------
if(JUDGE_TOOL_BUILD_TESTS)
    add_executable(judge_unit_tests
        tests/test_main.cpp
        tests/test_meta.cpp
        tests/test_compare.cpp
        tests/test_status.cpp
    )
    target_link_libraries(judge_unit_tests PRIVATE
        judge_core
        GTest::gtest_main
    )

    # 集成测试：跑真实 C++ 编译 / 运行
    add_executable(judge_integration_tests
        tests/test_main.cpp
        tests/test_runner_cpp.cpp
    )
    target_link_libraries(judge_integration_tests PRIVATE
        judge_core
        GTest::gtest_main
    )
    target_compile_definitions(judge_unit_tests        PRIVATE JUDGE_BIN_PATH=\"$<TARGET_FILE:judge>\")
    target_compile_definitions(judge_integration_tests PRIVATE JUDGE_BIN_PATH=\"$<TARGET_FILE:judge>\")
    add_test(NAME judge_unit_tests        COMMAND judge_unit_tests)
    add_test(NAME judge_integration_tests COMMAND judge_integration_tests)
endif()

# ----------------------------------------------------------------------------
#  Install —— 部署到 judge 镜像时拷贝
# ----------------------------------------------------------------------------
install(TARGETS judge RUNTIME DESTINATION bin)
