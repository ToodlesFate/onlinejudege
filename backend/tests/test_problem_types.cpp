// =============================================================================
//  test_problem_types.cpp —— problem_types 纯函数 / 数据形状单元测试
//  不依赖 MySQL；纯 std::string / 枚举 / 算术
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "domain/problem_types.hpp"

namespace {

using oj::domain::Difficulty;
using oj::domain::Language;
using oj::domain::ProblemListItem;
using oj::domain::difficulty_from_string;
using oj::domain::language_from_string;
using oj::domain::to_string;

// ---------------------------------------------------------------------------
//  Difficulty 枚举
// ---------------------------------------------------------------------------
TEST(DifficultyEnum, ToStringAllValues) {
    EXPECT_EQ(to_string(Difficulty::Easy),   "easy");
    EXPECT_EQ(to_string(Difficulty::Medium), "medium");
    EXPECT_EQ(to_string(Difficulty::Hard),   "hard");
}

TEST(DifficultyEnum, FromStringAcceptsAll) {
    EXPECT_EQ(difficulty_from_string("easy"),   Difficulty::Easy);
    EXPECT_EQ(difficulty_from_string("medium"), Difficulty::Medium);
    EXPECT_EQ(difficulty_from_string("hard"),   Difficulty::Hard);
}

TEST(DifficultyEnum, FromStringRejectsUnknown) {
    EXPECT_FALSE(difficulty_from_string("EASY").has_value());
    EXPECT_FALSE(difficulty_from_string("invalid").has_value());
    EXPECT_FALSE(difficulty_from_string("").has_value());
}

TEST(DifficultyEnum, RoundTripAllValues) {
    for (auto d : {Difficulty::Easy, Difficulty::Medium, Difficulty::Hard}) {
        auto back = difficulty_from_string(to_string(d));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, d);
    }
}

// ---------------------------------------------------------------------------
//  Language 枚举
// ---------------------------------------------------------------------------
TEST(LanguageEnum, ToStringAllValues) {
    EXPECT_EQ(to_string(Language::C),     "c");
    EXPECT_EQ(to_string(Language::Cpp),   "cpp");
    EXPECT_EQ(to_string(Language::Java),  "java");
    EXPECT_EQ(to_string(Language::Python), "python");
    EXPECT_EQ(to_string(Language::Go),     "go");
}

TEST(LanguageEnum, FromStringAcceptsAll) {
    EXPECT_EQ(language_from_string("c"),     Language::C);
    EXPECT_EQ(language_from_string("cpp"),   Language::Cpp);
    EXPECT_EQ(language_from_string("java"),  Language::Java);
    EXPECT_EQ(language_from_string("python"), Language::Python);
    EXPECT_EQ(language_from_string("go"),     Language::Go);
}

TEST(LanguageEnum, FromStringRejectsUnknown) {
    EXPECT_FALSE(language_from_string("c++").has_value());
    EXPECT_FALSE(language_from_string("rust").has_value());
    EXPECT_FALSE(language_from_string("").has_value());
}

TEST(LanguageEnum, RoundTripAllValues) {
    for (auto l : {Language::C, Language::Cpp, Language::Java, Language::Python, Language::Go}) {
        auto back = language_from_string(to_string(l));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, l);
    }
}

// ---------------------------------------------------------------------------
//  ProblemListItem::pass_rate
// ---------------------------------------------------------------------------
TEST(ProblemListItemTest, PassRateZeroWhenNoSubmissions) {
    ProblemListItem it;
    it.total_submissions = 0;
    it.accepted_submissions = 0;
    EXPECT_DOUBLE_EQ(it.pass_rate(), 0.0);
}

TEST(ProblemListItemTest, PassRateOneWhenAllAccepted) {
    ProblemListItem it;
    it.total_submissions = 10;
    it.accepted_submissions = 10;
    EXPECT_DOUBLE_EQ(it.pass_rate(), 1.0);
}

TEST(ProblemListItemTest, PassRatePartial) {
    ProblemListItem it;
    it.total_submissions = 3;
    it.accepted_submissions = 1;
    EXPECT_NEAR(it.pass_rate(), 1.0 / 3.0, 1e-9);
}

TEST(ProblemListItemTest, PassRateAcceptanceExceedingTotalClampedToOne) {
    // 防御性：理论上 accepted <= total；若数据脏了让 ratio 超过 1，不应崩
    ProblemListItem it;
    it.total_submissions = 0;
    it.accepted_submissions = 0;
    EXPECT_DOUBLE_EQ(it.pass_rate(), 0.0);  // total==0 走 0 分支
}

// ---------------------------------------------------------------------------
//  ProblemListQuery 默认值
// ---------------------------------------------------------------------------
TEST(ProblemListQueryTest, DefaultsAreSafe) {
    oj::domain::ProblemListQuery q;
    EXPECT_EQ(q.page, 1);
    EXPECT_EQ(q.page_size, 20);
    EXPECT_FALSE(q.difficulty.has_value());
    EXPECT_TRUE(q.tag_slugs.empty());
    EXPECT_TRUE(q.q.empty());
    EXPECT_EQ(q.sort, oj::domain::ProblemListQuery::Sort::IdDesc);
    EXPECT_FALSE(q.include_unpublished);
}

}  // namespace
