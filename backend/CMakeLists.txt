# =============================================================================
#  OnlineJudge Backend — 顶层 CMakeLists
#  依据 SPEC §3.2.1：Http / Domain / Infra / Common 四层；include 与 src 镜像
#  编译目标：
#    - oj_backend        ：主服务可执行文件
#    - oj_unit_tests     ：单元测试（doctest）— 仅 BUILD_TESTING=ON 时构建
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(onlinejudge_backend
        VERSION 1.0.0
        LANGUAGES CXX
        DESCRIPTION "OnlineJudge backend (C++20, cpp-httplib)")

# ---- 通用设置 ---------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---- 依赖（系统库 + FetchContent 第三方库）---------------------------------
# 系统库：libmysqlclient / libcurl / libargon2 / pthread
find_package(Threads            REQUIRED)
find_package(OpenSSL            REQUIRED)        # jwt-cpp / cpp-httplib TLS

find_package(PkgConfig          REQUIRED)
pkg_check_modules(MYSQLCLIENT    REQUIRED IMPORTED_TARGET mysqlclient)
pkg_check_modules(LIBCURL        REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(LIBARGON2      REQUIRED IMPORTED_TARGET libargon2)

# 允许通过 OJ_LOCAL_DEPS_DIR 指定本地已克隆的依赖目录，
# 便于在内网/无外网环境下跳过 FetchContent 的 git clone。
if(DEFINED OJ_LOCAL_DEPS_DIR AND IS_DIRECTORY "${OJ_LOCAL_DEPS_DIR}")
    set(OJ_DEPS_JSON_SRC   "${OJ_LOCAL_DEPS_DIR}/json")
    set(OJ_DEPS_HTTP_SRC   "${OJ_LOCAL_DEPS_DIR}/cpp-httplib")
    set(OJ_DEPS_JWT_SRC    "${OJ_LOCAL_DEPS_DIR}/jwt-cpp")
    set(OJ_DEPS_SPDLOG_SRC "${OJ_LOCAL_DEPS_DIR}/spdlog")
    set(OJ_DEPS_DOCTEST_SRC "${OJ_LOCAL_DEPS_DIR}/doctest")

    if(EXISTS "${OJ_DEPS_JSON_SRC}/single_include/nlohmann/json.hpp")
        add_library(oj_nlohmann_json INTERFACE)
        target_include_directories(oj_nlohmann_json INTERFACE "${OJ_DEPS_JSON_SRC}/single_include")
        add_library(nlohmann_json::nlohmann_json ALIAS oj_nlohmann_json)
    else()
        message(FATAL_ERROR "nlohmann/json not found under ${OJ_DEPS_JSON_SRC}")
    endif()

    if(EXISTS "${OJ_DEPS_HTTP_SRC}/httplib.h")
        add_library(oj_cpp_httplib INTERFACE)
        target_include_directories(oj_cpp_httplib INTERFACE "${OJ_DEPS_HTTP_SRC}")
        target_link_libraries(oj_cpp_httplib INTERFACE Threads::Threads)
        add_library(cpp_httplib::cpp_httplib ALIAS oj_cpp_httplib)
    else()
        message(FATAL_ERROR "cpp-httplib not found under ${OJ_DEPS_HTTP_SRC}")
    endif()

    if(EXISTS "${OJ_DEPS_JWT_SRC}/include/jwt-cpp/jwt.h")
        add_library(oj_jwt_cpp INTERFACE)
        target_include_directories(oj_jwt_cpp INTERFACE "${OJ_DEPS_JWT_SRC}/include")
        target_link_libraries(oj_jwt_cpp INTERFACE OpenSSL::SSL OpenSSL::Crypto)
        add_library(jwt-cpp::jwt-cpp ALIAS oj_jwt_cpp)
    else()
        message(FATAL_ERROR "jwt-cpp not found under ${OJ_DEPS_JWT_SRC}")
    endif()

    if(EXISTS "${OJ_DEPS_SPDLOG_SRC}/include/spdlog/spdlog.h")
        add_library(oj_spdlog INTERFACE)
        target_include_directories(oj_spdlog INTERFACE "${OJ_DEPS_SPDLOG_SRC}/include")
        add_library(spdlog::spdlog ALIAS oj_spdlog)
    else()
        message(FATAL_ERROR "spdlog not found under ${OJ_DEPS_SPDLOG_SRC}")
    endif()

    if(EXISTS "${OJ_DEPS_DOCTEST_SRC}/doctest/doctest.h")
        add_library(oj_doctest INTERFACE)
        target_include_directories(oj_doctest INTERFACE "${OJ_DEPS_DOCTEST_SRC}")
        add_library(doctest::doctest ALIAS oj_doctest)
    else()
        message(FATAL_ERROR "doctest not found under ${OJ_DEPS_DOCTEST_SRC}")
    endif()

    message(STATUS "OJ backend: using local deps at ${OJ_LOCAL_DEPS_DIR}")
else()
    include(${CMAKE_SOURCE_DIR}/cmake/deps.cmake)   # FetchContent：nlohmann_json / cpp_httplib / jwt_cpp / spdlog / doctest
endif()

# ---- 全局编译选项 -----------------------------------------------------------
add_library(oj_warnings INTERFACE)
target_compile_options(oj_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
    -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion
    -Wformat=2
)
target_compile_options(oj_warnings INTERFACE
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# ---- 公共源文件收集（include 与 src 镜像）----------------------------------
file(GLOB OJ_COMMON_SRC
     "${CMAKE_SOURCE_DIR}/src/common/*.cpp")
file(GLOB OJ_COMMON_INC
     "${CMAKE_SOURCE_DIR}/include/common/*.hpp")

file(GLOB OJ_HTTP_SRC
     "${CMAKE_SOURCE_DIR}/src/http/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/http/handlers/*.cpp"
     "${CMAKE_SOURCE_DIR}/src/http/middleware/*.cpp")
file(GLOB OJ_HTTP_INC
     "${CMAKE_SOURCE_DIR}/include/http/*.hpp"
     "${CMAKE_SOURCE_DIR}/include/http/handlers/*.hpp"
     "${CMAKE_SOURCE_DIR}/include/http/middleware/*.hpp")

file(GLOB OJ_DOMAIN_SRC
     "${CMAKE_SOURCE_DIR}/src/domain/*.cpp")
file(GLOB OJ_DOMAIN_INC
     "${CMAKE_SOURCE_DIR}/include/domain/*.hpp")

file(GLOB OJ_INFRA_SRC
     "${CMAKE_SOURCE_DIR}/src/infra/*.cpp")
file(GLOB OJ_INFRA_INC
     "${CMAKE_SOURCE_DIR}/include/infra/*.hpp")

# ---- 核心静态库：oj_core (供主程序 + 测试共用) -----------------------------
add_library(oj_core STATIC
    ${OJ_COMMON_SRC}
    ${OJ_HTTP_SRC}
    ${OJ_DOMAIN_SRC}
    ${OJ_INFRA_SRC}
)
target_include_directories(oj_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
        ${MYSQLCLIENT_INCLUDE_DIRS}
        ${LIBCURL_INCLUDE_DIRS}
        ${LIBARGON2_INCLUDE_DIRS}
)
target_link_libraries(oj_core
    PUBLIC
        oj_warnings
        nlohmann_json::nlohmann_json
        cpp_httplib::cpp_httplib
        jwt-cpp::jwt-cpp
        spdlog::spdlog
        PkgConfig::MYSQLCLIENT
        PkgConfig::LIBCURL
        PkgConfig::LIBARGON2
        OpenSSL::SSL OpenSSL::Crypto
        Threads::Threads
)

# ---- 主可执行文件：oj_backend ----------------------------------------------
add_executable(oj_backend
    src/main.cpp
)
target_link_libraries(oj_backend PRIVATE oj_core)

# 运行时配置文件挂到二进制旁边（便于容器内通过 -cwd 切换覆盖）
configure_file(
    ${CMAKE_SOURCE_DIR}/config/default.json
    ${CMAKE_BINARY_DIR}/config/default.json
    COPYONLY
)

# 启动时打印版本信息（便于 AC-1 / AC-2 验收时核对）
target_compile_definitions(oj_backend PRIVATE
    OJ_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    OJ_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    OJ_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# ---- 单元测试（doctest） — 默认开启 ----------------------------------------
option(OJ_BUILD_TESTS "Build backend unit tests" ON)
if(OJ_BUILD_TESTS)
    enable_testing()

    file(GLOB OJ_TEST_SRC
         "${CMAKE_SOURCE_DIR}/tests/*.cpp")

    add_executable(oj_unit_tests ${OJ_TEST_SRC})
    target_link_libraries(oj_unit_tests
        PRIVATE
            oj_core
            oj_warnings
            doctest::doctest
    )

    add_test(NAME oj_unit_tests COMMAND oj_unit_tests)
endif()