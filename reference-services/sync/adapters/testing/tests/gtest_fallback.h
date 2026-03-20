#pragma once

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace ifex {
namespace sync {
namespace testing {
namespace fallback {

struct TestCase {
    std::string suite_name;
    std::string test_name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline std::string& current_test_name() {
    static std::string name;
    return name;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

inline void register_test(const char* suite_name, const char* test_name, void (*fn)()) {
    registry().push_back(TestCase{suite_name, test_name, fn});
}

inline void record_failure(const char* file,
                           int line,
                           const std::string& expectation,
                           const char* lhs,
                           const char* rhs) {
    std::cerr << file << ':' << line << ": " << current_test_name() << ": "
              << expectation << " failed (" << lhs << ", " << rhs << ")" << std::endl;
    ++failure_count();
}

inline void record_boolean_failure(const char* file,
                                   int line,
                                   const std::string& expectation,
                                   const char* expression) {
    std::cerr << file << ':' << line << ": " << current_test_name() << ": "
              << expectation << " failed (" << expression << ')' << std::endl;
    ++failure_count();
}

template <typename Left, typename Right>
inline void expect_eq(const Left& lhs,
                      const Right& rhs,
                      const char* lhs_text,
                      const char* rhs_text,
                      const char* file,
                      int line) {
    if (!(lhs == rhs)) {
        record_failure(file, line, "EXPECT_EQ", lhs_text, rhs_text);
    }
}

template <typename Left, typename Right>
inline void expect_ne(const Left& lhs,
                      const Right& rhs,
                      const char* lhs_text,
                      const char* rhs_text,
                      const char* file,
                      int line) {
    if (!(lhs != rhs)) {
        record_failure(file, line, "EXPECT_NE", lhs_text, rhs_text);
    }
}

inline void expect_true(bool value, const char* expression, const char* file, int line) {
    if (!value) {
        record_boolean_failure(file, line, "EXPECT_TRUE", expression);
    }
}

inline void expect_false(bool value, const char* expression, const char* file, int line) {
    if (value) {
        record_boolean_failure(file, line, "EXPECT_FALSE", expression);
    }
}

inline bool wildcard_match(const char* pattern, const char* value) {
    if (*pattern == '\0') {
        return *value == '\0';
    }
    if (*pattern == '*') {
        return wildcard_match(pattern + 1, value) ||
               (*value != '\0' && wildcard_match(pattern, value + 1));
    }
    if (*value == '\0') {
        return false;
    }
    return *pattern == *value && wildcard_match(pattern + 1, value + 1);
}

inline std::string parse_filter(int argc, char** argv) {
    const std::string prefix = "--gtest_filter=";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.compare(0, prefix.size(), prefix) == 0) {
            return argument.substr(prefix.size());
        }
    }
    return "*";
}

inline int run_all(int argc, char** argv) {
    const std::string filter = parse_filter(argc, argv);
    int matched_tests = 0;
    for (const TestCase& test_case : registry()) {
        const std::string full_name = test_case.suite_name + "." + test_case.test_name;
        if (!wildcard_match(filter.c_str(), full_name.c_str())) {
            continue;
        }
        current_test_name() = full_name;
        test_case.fn();
        ++matched_tests;
    }

    if (matched_tests == 0) {
        std::cerr << "No fallback tests matched filter " << filter << std::endl;
        return 1;
    }
    return failure_count() == 0 ? 0 : 1;
}

}
}
}
}

#define TEST(suite_name, test_name)                                                \
    static void TEST_##suite_name##_##test_name();                                 \
    namespace {                                                                    \
    struct TEST_REGISTRAR_##suite_name##_##test_name {                             \
        TEST_REGISTRAR_##suite_name##_##test_name() {                              \
            ::ifex::sync::testing::fallback::register_test(                        \
                #suite_name, #test_name, &TEST_##suite_name##_##test_name);        \
        }                                                                          \
    } TEST_REGISTRAR_INSTANCE_##suite_name##_##test_name;                          \
    }                                                                              \
    static void TEST_##suite_name##_##test_name()

#define EXPECT_EQ(lhs, rhs)                                                        \
    do {                                                                           \
        ::ifex::sync::testing::fallback::expect_eq(                                \
            (lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__);                         \
    } while (false)

#define EXPECT_NE(lhs, rhs)                                                        \
    do {                                                                           \
        ::ifex::sync::testing::fallback::expect_ne(                                \
            (lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__);                         \
    } while (false)

#define EXPECT_TRUE(expression)                                                    \
    do {                                                                           \
        ::ifex::sync::testing::fallback::expect_true(                              \
            static_cast<bool>(expression), #expression, __FILE__, __LINE__);       \
    } while (false)

#define EXPECT_FALSE(expression)                                                   \
    do {                                                                           \
        ::ifex::sync::testing::fallback::expect_false(                             \
            static_cast<bool>(expression), #expression, __FILE__, __LINE__);       \
    } while (false)

#define GTEST_SKIP() return
