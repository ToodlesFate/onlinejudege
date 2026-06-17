// =============================================================================
//  tests/test_meta.cpp —— meta.json 解析
// =============================================================================
#include <gtest/gtest.h>
#include "meta.hpp"
#include <cstdio>
#include <fstream>
#include <string>

namespace {

class MetaTest : public ::testing::Test {
protected:
    std::string tmp_path_;

    void TearDown() override {
        if (!tmp_path_.empty()) std::remove(tmp_path_.c_str());
    }

    std::string write_meta(const std::string& json_text) {
        char path[] = "/tmp/oj_meta_XXXXXX";
        int fd = mkstemp(path);
        if (fd < 0) {
            ADD_FAILURE() << "mkstemp failed";
            return {};
        }
        ::close(fd);
        std::ofstream f(path);
        f << json_text;
        f.close();
        tmp_path_ = path;
        return path;
    }
};

TEST_F(MetaTest, ValidCpp) {
    auto p = write_meta(R"({"language":"cpp","time_limit_ms":2000,"memory_limit_mb":256,"output_limit_mb":64})");
    std::string err;
    auto L = judge::read_meta_file(p, &err);
    ASSERT_TRUE(L) << err;
    EXPECT_EQ(L->language, judge::Language::Cpp);
    EXPECT_EQ(L->time_ms, 2000);
    EXPECT_EQ(L->mem_mb,  256);
    EXPECT_EQ(L->out_mb,  64);
}

TEST_F(MetaTest, AllFiveLanguages) {
    for (const auto& lang : {"c", "cpp", "java", "python", "go"}) {
        auto p = write_meta(std::string("{\"language\":\"") + lang + "\"}");
        std::string err;
        auto L = judge::read_meta_file(p, &err);
        ASSERT_TRUE(L) << lang << ": " << err;
        EXPECT_EQ(judge::language_id(L->language), lang);
    }
}

TEST_F(MetaTest, MissingLanguage) {
    auto p = write_meta(R"({"time_limit_ms":2000})");
    std::string err;
    auto L = judge::read_meta_file(p, &err);
    EXPECT_FALSE(L);
    EXPECT_NE(err.find("language"), std::string::npos);
}

TEST_F(MetaTest, UnknownLanguage) {
    auto p = write_meta(R"({"language":"rust"})");
    std::string err;
    auto L = judge::read_meta_file(p, &err);
    EXPECT_FALSE(L);
    EXPECT_NE(err.find("unsupported"), std::string::npos);
}

TEST_F(MetaTest, TimeLimitOutOfRange) {
    auto p = write_meta(R"({"language":"cpp","time_limit_ms":0})");
    std::string err;
    EXPECT_FALSE(judge::read_meta_file(p, &err));
    EXPECT_NE(err.find("time_limit_ms"), std::string::npos);
}

TEST_F(MetaTest, TimeLimitTooLarge) {
    auto p = write_meta(R"({"language":"cpp","time_limit_ms":60000})");
    std::string err;
    EXPECT_FALSE(judge::read_meta_file(p, &err));
}

TEST_F(MetaTest, MemoryLimitOutOfRange) {
    auto p = write_meta(R"({"language":"cpp","memory_limit_mb":4})");
    std::string err;
    EXPECT_FALSE(judge::read_meta_file(p, &err));
}

TEST_F(MetaTest, FileNotExist) {
    std::string err;
    auto L = judge::read_meta_file("/tmp/oj_no_such_file_xxxxxx.json", &err);
    EXPECT_FALSE(L);
    EXPECT_NE(err.find("cannot open"), std::string::npos);
}

TEST_F(MetaTest, MalformedJson) {
    auto p = write_meta("{not json}");
    std::string err;
    EXPECT_FALSE(judge::read_meta_file(p, &err));
}

}  // namespace
