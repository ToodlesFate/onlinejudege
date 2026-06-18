// =============================================================================
//  test_submission_state_machine.cpp
//  单元测试 —— SubmissionStatus / SubmissionResult 状态机辅助函数
//  (SPEC §2.3.2)
//
//  覆盖：
//    1) to_string / submission_status_from_string 双向稳定（4 态）
//    2) to_string / submission_result_from_string 双向稳定（8 态）
//    3) is_terminal_status：4 态中只有 Finished=true
//    4) is_terminal(SubmissionResult)：8 态全部 true
//    5) is_early_exit：仅 CE / SE 返 true（早退出），其余 false
//    6) 状态机转移的合法性检查：status 字符串解析失败返 nullopt
//    7) round-trip：任意 enum 都能 to_string → from_string 还原
//    8) 防御：未知字符串 from_string 返 nullopt
//    9) 与 8 态结果枚举对应的中文/英文标签常量（用于前端/日志一致性）
//
//  所有用例不依赖 MySQL / Docker，纯领域类型断言；通过 gtest 串入现有
//  oj_unit_tests 可执行。
// =============================================================================

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

#include "domain/submission_types.hpp"

namespace {

using oj::domain::SubmissionResult;
using oj::domain::SubmissionStatus;

constexpr std::array<SubmissionStatus, 4> kAllStatus = {
    SubmissionStatus::Queued,
    SubmissionStatus::Compiling,
    SubmissionStatus::Running,
    SubmissionStatus::Finished,
};

constexpr std::array<SubmissionResult, 8> kAllResults = {
    SubmissionResult::AC,
    SubmissionResult::WA,
    SubmissionResult::TLE,
    SubmissionResult::MLE,
    SubmissionResult::OLE,
    SubmissionResult::RE,
    SubmissionResult::CE,
    SubmissionResult::SE,
};

// ---------------------------------------------------------------------------
//  SubmissionStatus ↔ string —— 双向稳定
// ---------------------------------------------------------------------------
TEST(SubmissionStatusStrTest, ToStringCoversAllStates) {
    EXPECT_EQ(oj::domain::to_string(SubmissionStatus::Queued),    "queued");
    EXPECT_EQ(oj::domain::to_string(SubmissionStatus::Compiling), "compiling");
    EXPECT_EQ(oj::domain::to_string(SubmissionStatus::Running),   "running");
    EXPECT_EQ(oj::domain::to_string(SubmissionStatus::Finished),  "finished");
}

TEST(SubmissionStatusStrTest, FromStringRecognizesAllStates) {
    EXPECT_EQ(oj::domain::submission_status_from_string("queued"),
              SubmissionStatus::Queued);
    EXPECT_EQ(oj::domain::submission_status_from_string("compiling"),
              SubmissionStatus::Compiling);
    EXPECT_EQ(oj::domain::submission_status_from_string("running"),
              SubmissionStatus::Running);
    EXPECT_EQ(oj::domain::submission_status_from_string("finished"),
              SubmissionStatus::Finished);
}

TEST(SubmissionStatusStrTest, FromStringUnknownReturnsNullopt) {
    EXPECT_FALSE(oj::domain::submission_status_from_string("").has_value());
    EXPECT_FALSE(oj::domain::submission_status_from_string("foo").has_value());
    EXPECT_FALSE(oj::domain::submission_status_from_string("Queued").has_value());  // 大小写敏感
    EXPECT_FALSE(oj::domain::submission_status_from_string("queued ").has_value()); // 尾空格
    EXPECT_FALSE(oj::domain::submission_status_from_string("finish").has_value());
}

TEST(SubmissionStatusStrTest, RoundTripStableForAllStates) {
    for (auto s : kAllStatus) {
        const std::string_view sv = oj::domain::to_string(s);
        const auto back = oj::domain::submission_status_from_string(sv);
        ASSERT_TRUE(back.has_value()) << "round-trip lost for status=" << sv;
        EXPECT_EQ(*back, s);
    }
}

// ---------------------------------------------------------------------------
//  SubmissionResult ↔ string
// ---------------------------------------------------------------------------
TEST(SubmissionResultStrTest, ToStringCoversAllEightResults) {
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::AC),  "AC");
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::WA),  "WA");
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::TLE), "TLE");
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::MLE), "MLE");
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::OLE), "OLE");
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::RE),  "RE");
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::CE),  "CE");
    EXPECT_EQ(oj::domain::to_string(SubmissionResult::SE),  "SE");
}

TEST(SubmissionResultStrTest, FromStringRecognizesAllEightResults) {
    EXPECT_EQ(oj::domain::submission_result_from_string("AC"),  SubmissionResult::AC);
    EXPECT_EQ(oj::domain::submission_result_from_string("WA"),  SubmissionResult::WA);
    EXPECT_EQ(oj::domain::submission_result_from_string("TLE"), SubmissionResult::TLE);
    EXPECT_EQ(oj::domain::submission_result_from_string("MLE"), SubmissionResult::MLE);
    EXPECT_EQ(oj::domain::submission_result_from_string("OLE"), SubmissionResult::OLE);
    EXPECT_EQ(oj::domain::submission_result_from_string("RE"),  SubmissionResult::RE);
    EXPECT_EQ(oj::domain::submission_result_from_string("CE"),  SubmissionResult::CE);
    EXPECT_EQ(oj::domain::submission_result_from_string("SE"),  SubmissionResult::SE);
}

TEST(SubmissionResultStrTest, FromStringUnknownReturnsNullopt) {
    EXPECT_FALSE(oj::domain::submission_result_from_string("").has_value());
    EXPECT_FALSE(oj::domain::submission_result_from_string("ac").has_value());   // 小写
    EXPECT_FALSE(oj::domain::submission_result_from_string("AC ").has_value());  // 空格
    EXPECT_FALSE(oj::domain::submission_result_from_string("XX").has_value());
    EXPECT_FALSE(oj::domain::submission_result_from_string("queued").has_value());
}

TEST(SubmissionResultStrTest, RoundTripStableForAllResults) {
    for (auto r : kAllResults) {
        const std::string_view sv = oj::domain::to_string(r);
        const auto back = oj::domain::submission_result_from_string(sv);
        ASSERT_TRUE(back.has_value()) << "round-trip lost for result=" << sv;
        EXPECT_EQ(*back, r);
    }
}

// ---------------------------------------------------------------------------
//  is_terminal_status —— 主流程 4 态中只有 Finished=true
// ---------------------------------------------------------------------------
TEST(IsTerminalStatusTest, OnlyFinishedIsTerminal) {
    EXPECT_FALSE(oj::domain::is_terminal_status(SubmissionStatus::Queued));
    EXPECT_FALSE(oj::domain::is_terminal_status(SubmissionStatus::Compiling));
    EXPECT_FALSE(oj::domain::is_terminal_status(SubmissionStatus::Running));
    EXPECT_TRUE (oj::domain::is_terminal_status(SubmissionStatus::Finished));
}

TEST(IsTerminalStatusTest, MatchesStatusStringRepresentation) {
    // to_string("finished") 对应唯一 is_terminal_status=true 的状态
    for (auto s : kAllStatus) {
        const std::string_view sv = oj::domain::to_string(s);
        const bool expected = (sv == "finished");
        EXPECT_EQ(oj::domain::is_terminal_status(s), expected)
            << "status=" << sv;
    }
}

// ---------------------------------------------------------------------------
//  is_terminal(SubmissionResult) —— 8 态全部 true
// ---------------------------------------------------------------------------
TEST(IsTerminalResultTest, AllEightResultsAreTerminal) {
    for (auto r : kAllResults) {
        EXPECT_TRUE(oj::domain::is_terminal(r))
            << "result=" << oj::domain::to_string(r);
    }
}

// ---------------------------------------------------------------------------
//  is_early_exit —— 仅 CE / SE 返 true
// ---------------------------------------------------------------------------
TEST(IsEarlyExitTest, OnlyCEAndSEAreEarlyExit) {
    EXPECT_TRUE (oj::domain::is_early_exit(SubmissionResult::CE));
    EXPECT_TRUE (oj::domain::is_early_exit(SubmissionResult::SE));

    EXPECT_FALSE(oj::domain::is_early_exit(SubmissionResult::AC));
    EXPECT_FALSE(oj::domain::is_early_exit(SubmissionResult::WA));
    EXPECT_FALSE(oj::domain::is_early_exit(SubmissionResult::TLE));
    EXPECT_FALSE(oj::domain::is_early_exit(SubmissionResult::MLE));
    EXPECT_FALSE(oj::domain::is_early_exit(SubmissionResult::OLE));
    EXPECT_FALSE(oj::domain::is_early_exit(SubmissionResult::RE));
}

TEST(IsEarlyExitTest, EarlyExitExactlyTwoOfEight) {
    // 8 态中恰好 2 态为 early-exit（CE、SE）
    int cnt = 0;
    for (auto r : kAllResults) {
        if (oj::domain::is_early_exit(r)) cnt++;
    }
    EXPECT_EQ(cnt, 2);
}

// ---------------------------------------------------------------------------
//  is_early_exit 与 is_terminal 的关系
// ---------------------------------------------------------------------------
TEST(SubmissionStateMachineTest, EarlyExitIsAlwaysTerminal) {
    for (auto r : kAllResults) {
        if (oj::domain::is_early_exit(r)) {
            EXPECT_TRUE(oj::domain::is_terminal(r))
                << "early-exit 也应是终态：result=" << oj::domain::to_string(r);
        }
    }
}

// ---------------------------------------------------------------------------
//  状态机一致性：to_string 不会产生与 from_string 不识别的字符串
// ---------------------------------------------------------------------------
TEST(SubmissionStateMachineTest, ToStringAlwaysRoundTrippable) {
    for (auto s : kAllStatus) {
        const std::string_view out = oj::domain::to_string(s);
        const auto back = oj::domain::submission_status_from_string(out);
        ASSERT_TRUE(back.has_value()) << "to_string produced unparseable string: " << out;
        EXPECT_EQ(*back, s);
    }
    for (auto r : kAllResults) {
        const std::string_view out = oj::domain::to_string(r);
        const auto back = oj::domain::submission_result_from_string(out);
        ASSERT_TRUE(back.has_value()) << "to_string produced unparseable string: " << out;
        EXPECT_EQ(*back, r);
    }
}

// ---------------------------------------------------------------------------
//  状态机一致性：from_string("finished") + 8 态 result 组合 = valid output
//  （即 SPEC §5.3 响应示例的语义对得上）
// ---------------------------------------------------------------------------
TEST(SubmissionStateMachineTest, FinishedPlusResultIsValidApiShape) {
    // 模拟一次 finished+AC：应能被 is_terminal_status + is_terminal 同时认可
    const SubmissionStatus s = SubmissionStatus::Finished;
    const SubmissionResult r = SubmissionResult::AC;
    EXPECT_TRUE(oj::domain::is_terminal_status(s));
    EXPECT_TRUE(oj::domain::is_terminal(r));
    EXPECT_FALSE(oj::domain::is_early_exit(r));

    // finished + CE：早退出
    EXPECT_TRUE(oj::domain::is_terminal_status(s));
    EXPECT_TRUE(oj::domain::is_terminal(SubmissionResult::CE));
    EXPECT_TRUE(oj::domain::is_early_exit(SubmissionResult::CE));
}

// ---------------------------------------------------------------------------
//  8 态字符串与前端 badge / 状态机可视化标签严格一致
//  （前端 js/components/status-machine.js 与 utils/dom.js#statusBadge
//   用的就是 to_string 的返回值）
// ---------------------------------------------------------------------------
TEST(SubmissionStateMachineTest, ResultStringsMatchFrontendContract) {
    // 8 个串分别对应前端 SPEC §3.3.5 J 颜色映射的 key
    const std::string_view expected[] = {
        "AC", "WA", "TLE", "MLE", "OLE", "RE", "CE", "SE"
    };
    for (std::size_t i = 0; i < kAllResults.size(); ++i) {
        EXPECT_EQ(oj::domain::to_string(kAllResults[i]), expected[i])
            << "i=" << i;
    }
}

// SPEC §9.1.3 AC-8/9/10/11/12: 8 种终态在状态机里合法出现;
// 防御性:to_string 任意 enum 都必须落在一个非空字符串上,不允许空串或 nullopt。
TEST(SubmissionStateMachineTest, ToStringNeverEmpty) {
    for (auto s : kAllStatus) {
        const std::string_view sv = oj::domain::to_string(s);
        EXPECT_FALSE(sv.empty());
    }
    for (auto r : kAllResults) {
        const std::string_view sv = oj::domain::to_string(r);
        EXPECT_FALSE(sv.empty());
    }
}

// SPEC §2.3.2 状态机: queued -> compiling -> running -> finished;
// queued / compiling / running 都不是终态,只能 finished 配 8 态 result。
// 此处校验:"任意非 Finished 状态 is_terminal 必须为 false",
// 防止新加状态时漏掉 is_terminal_status 维护。
TEST(IsTerminalStatusTest, OnlyExactlyOneStatusIsTerminal) {
    int terminal_count = 0;
    for (auto s : kAllStatus) {
        if (oj::domain::is_terminal_status(s)) terminal_count++;
    }
    EXPECT_EQ(terminal_count, 1)
        << "提交状态 4 态中应只有 finished 一个终态";
}

// SPEC §2.3.2 状态机:8 种 result 全部是终态。
TEST(IsTerminalResultTest, ExactlyEightResultsAreTerminal) {
    int terminal_count = 0;
    for (auto r : kAllResults) {
        if (oj::domain::is_terminal(r)) terminal_count++;
    }
    EXPECT_EQ(terminal_count, 8)
        << "8 种结果全部是终态,不应新增或遗漏";
}

}  // namespace
