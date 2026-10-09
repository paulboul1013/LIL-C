/*
 * Checks the test framework itself. Every test in this file fails on
 * purpose (except one); tests/check_framework.sh verifies that the binary
 * exits nonzero, keeps running after failures, and prints file, line,
 * expected and actual values.
 */
#include "test_framework.h"

static int reached_after_expect_failure;
static int reached_after_assert_failure;

TEST(selftest_int_mismatch) {
    EXPECT_EQ(42, 41);
    reached_after_expect_failure = 1;
}

TEST(selftest_string_mismatch) {
    EXPECT_STR_EQ("expected text", "actual\ttext");
}

TEST(selftest_null_string) {
    EXPECT_STR_EQ("x", NULL);
}

static int evaluations;

static int counted(const char *text) {
    (void)text;
    ++evaluations;
    return 0;
}

static const char *counted_text(void) {
    ++evaluations;
    return "abc";
}

TEST(selftest_unsigned_mismatch) {
    EXPECT_EQ(UINT64_MAX, (uint64_t)UINT64_MAX - 1);
}

TEST(selftest_assert_stops_test) {
    ASSERT_TRUE(1 == 2);
    reached_after_assert_failure = 1;
}

TEST(selftest_passing) {
    EXPECT_EQ(7, 7);
    EXPECT_STR_EQ("same", "same");
    EXPECT_MEM_EQ("abc", "abcdef", 3);
    EXPECT_TRUE(reached_after_expect_failure == 1);
    EXPECT_TRUE(reached_after_assert_failure == 0);
    EXPECT_EQ(UINT64_MAX, (uint64_t)UINT64_MAX);
    EXPECT_EQ(-1, (int64_t)-1);

    /* Every operand is evaluated exactly once. */
    evaluations = 0;
    EXPECT_EQ(0, counted("x"));
    EXPECT_MEM_EQ(counted_text(), "abc", 3);
    EXPECT_STR_EQ(counted_text(), "abc");
    EXPECT_TRUE(evaluations == 3);
}

int main(void) {
    RUN_TEST(selftest_int_mismatch);
    RUN_TEST(selftest_string_mismatch);
    RUN_TEST(selftest_null_string);
    RUN_TEST(selftest_unsigned_mismatch);
    RUN_TEST(selftest_assert_stops_test);
    RUN_TEST(selftest_passing);
    return test_finish();
}
