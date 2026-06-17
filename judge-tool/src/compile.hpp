#pragma once
#include "judge/types.hpp"
#include <string>

namespace judge {

struct CompileResult {
    bool        ok = false;
    int         exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

// 编译源代码到 working_dir/bin（编译型）或 working_dir/bin/...（Java）。
//   - src_dir:   源文件所在目录
//   - working_dir: 编译产物输出目录（通常 = src_dir 或 out_dir）
//
// 编译失败 → ok=false；调用方应把 stdout+stderr 写入 compile.log 并退出 10。
CompileResult compile(Language lang,
                      const std::string& src_dir,
                      const std::string& working_dir,
                      int mem_mb);

// 把 run.sh / build 模板写到一个临时文件
std::string make_build_script(Language lang,
                              const std::string& src_dir,
                              const std::string& working_dir,
                              int mem_mb);

}  // namespace judge
