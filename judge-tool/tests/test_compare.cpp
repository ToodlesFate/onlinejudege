// =============================================================================
//  tests/test_compare.cpp —— diff -b 行为
// =============================================================================
#include <gtest/gtest.h>
#include "compare.hpp"

using judge::compare_outputs;

TEST(Compare, IdenticalIsMatch) {
    EXPECT_TRUE(compare_outputs("hello\nworld\n", "hello\nworld\n"));
}

TEST(Compare, TrailingSpacesIgnored) {
    EXPECT_TRUE(compare_outputs("hello   \nworld\t\t\n", "hello\nworld\n"));
}

TEST(Compare, TrailingCRIgnored) {
    EXPECT_TRUE(compare_outputs("hello\r\nworld\r\n", "hello\nworld\n"));
}

TEST(Compare, LineCountDiffers) {
    std::string d;
    EXPECT_FALSE(compare_outputs("a\nb\nc\n", "a\nb\n", &d));
    EXPECT_NE(d.find("line count"), std::string::npos);
}

TEST(Compare, DifferentContent) {
    std::string d;
    EXPECT_FALSE(compare_outputs("foo\nbar\n", "foo\nbaz\n", &d));
    EXPECT_NE(d.find("line 2"), std::string::npos);
    EXPECT_NE(d.find("expected=\"bar\""), std::string::npos);
    EXPECT_NE(d.find("actual=\"baz\""), std::string::npos);
}

TEST(Compare, InternalSpacesCounted) {
    // diff -b 不会忽略行内空白差异
    EXPECT_FALSE(compare_outputs("a b\n", "a  b\n"));
}

TEST(Compare, EmptyStringsAreEqual) {
    EXPECT_TRUE(compare_outputs("", ""));
}

TEST(Compare, ExtraTrailingNewlineIsDifferent) {
    // "a\n" vs "a\n\n" → 第二个 split 出 ["a", ""] vs ["a"] → 长度不同
    EXPECT_FALSE(compare_outputs("a\n", "a\n\n"));
}

TEST(Compare, DiffMessageTruncatesLongLines) {
    std::string expected = "a\n";
    std::string actual   = std::string(200, 'x') + "\n";
    std::string d;
    EXPECT_FALSE(compare_outputs(expected, actual, &d));
    EXPECT_NE(d.find("..."), std::string::npos);
}
