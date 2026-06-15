# deps.cmake
# ---------------------------------------------------------------------------
# 集中通过 FetchContent 拉取所有第三方依赖。
# 仅使用源码库（header-only / 静态编译），避免污染宿主机。
# 在顶层 CMakeLists.txt 中通过 include(${CMAKE_SOURCE_DIR}/cmake/deps.cmake) 引入。
# ---------------------------------------------------------------------------

include(FetchContent)

# 关闭所有子项目的 install / 测试 / 例子，缩短构建时间
set(FETCHCONTENT_QUIET OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)

# ---- nlohmann/json (header-only JSON) ---------------------------------------
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
set(JSON_BuildTests       OFF CACHE INTERNAL "")
set(JSON_Install          OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ---- cpp-httplib (header-only HTTP server) ---------------------------------
FetchContent_Declare(
  cpp_httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.15.3
  GIT_SHALLOW    TRUE
)
set(HTTPLIB_COMPILE        OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(cpp_httplib)

# ---- jwt-cpp (header-only JWT, depends on OpenSSL) -------------------------
FetchContent_Declare(
  jwt_cpp
  GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
  GIT_TAG        v0.7.0
  GIT_SHALLOW    TRUE
)
set(JWT_BUILD_EXAMPLES     OFF CACHE INTERNAL "")
set(JWT_DISABLE_PICOJSON   ON  CACHE INTERNAL "")
set(JWT_DISABLE_BASE64     OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(jwt_cpp)

# ---- spdlog (header + compiled, async logger) ------------------------------
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.13.0
  GIT_SHALLOW    TRUE
)
set(SPDLOG_BUILD_EXAMPLES  OFF CACHE INTERNAL "")
set(SPDLOG_BUILD_TESTS     OFF CACHE INTERNAL "")
set(SPDLOG_INSTALL         OFF CACHE INTERNAL "")
set(SPDLOG_FMT_EXTERNAL    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(spdlog)

# ---- doctest (unit test framework) -----------------------------------------
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    TRUE
)
set(DOCTEST_WITH_TESTS     OFF CACHE INTERNAL "")
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(doctest)
