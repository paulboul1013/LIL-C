#ifndef LIL_TEST_FRAMEWORK_H
#define LIL_TEST_FRAMEWORK_H

/*
 * Minimal single-translation-unit unit test framework.
 *
 *     TEST(lexer_splits_number_and_name) {
 *         EXPECT_EQ(TOKEN_NUMBER, token.kind);
 *         EXPECT_STR_EQ("unexpected character '@'", diagnostic->message);
 *     }
 *
 *     int main(int argc, char **argv) {
 *         RUN_TEST(lexer_splits_number_and_name);
 *         return test_finish();
 *     }
 *
 * EXPECT_* records a failure (test name, file, line, expected and actual)
 * and keeps running. ASSERT_* does the same and then returns from the
 * current test function, for checks whose failure would make the rest of
 * the test crash (for example a NULL pointer). ASSERT_* does not release
 * anything the test already acquired, so use it only before the first
 * allocation; afterwards guard the rest of the test with an 'if' instead.
 *
 * Every TEST() runs even when earlier tests fail. test_finish() prints a
 * summary and returns a nonzero exit status when anything failed.
 * A test filter can be given with test_set_filter(): only tests whose name
 * contains that substring are run.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *current_test;
    const char *filter;
    bool current_failed;
    size_t tests_run;
    size_t tests_failed;
    size_t tests_skipped;
    size_t checks_failed;
} Test_State;

static Test_State test_state;

#define TEST(name) static void name(void)

#define RUN_TEST(name) test_run(#name, name)

static inline void test_set_filter(const char *filter) {
    test_state.filter = filter;
}

static inline void test_run(const char *name, void (*function)(void)) {
    if (test_state.filter != NULL && strstr(name, test_state.filter) == NULL) {
        ++test_state.tests_skipped;
        return;
    }

    test_state.current_test = name;
    test_state.current_failed = false;
    ++test_state.tests_run;

    function();

    if (test_state.current_failed) {
        ++test_state.tests_failed;
        printf("[ FAIL ] %s\n", name);
    } else {
        printf("[  OK  ] %s\n", name);
    }

    test_state.current_test = NULL;
}

static inline void test_report_failure_header(const char *file, int line) {
    test_state.current_failed = true;
    ++test_state.checks_failed;
    fprintf(
        stderr,
        "%s:%d: test '%s' failed\n",
        file,
        line,
        test_state.current_test != NULL ? test_state.current_test : "(none)"
    );
}

static inline bool test_check_true(
    bool condition,
    const char *expression,
    const char *file,
    int line
) {
    if (condition) {
        return true;
    }

    test_report_failure_header(file, line);
    fprintf(stderr, "    expected: %s to be true\n", expression);
    fprintf(stderr, "    actual:   false\n");
    return false;
}

static inline bool test_check_int(
    intmax_t expected,
    intmax_t actual,
    const char *expected_expression,
    const char *actual_expression,
    const char *file,
    int line
) {
    if (expected == actual) {
        return true;
    }

    test_report_failure_header(file, line);
    fprintf(
        stderr,
        "    expected: %" PRIdMAX " (%s)\n"
        "    actual:   %" PRIdMAX " (%s)\n",
        expected,
        expected_expression,
        actual,
        actual_expression
    );
    return false;
}

static inline bool test_check_uint(
    uintmax_t expected,
    uintmax_t actual,
    const char *expected_expression,
    const char *actual_expression,
    const char *file,
    int line
) {
    if (expected == actual) {
        return true;
    }

    test_report_failure_header(file, line);
    fprintf(
        stderr,
        "    expected: %" PRIuMAX " (%s)\n"
        "    actual:   %" PRIuMAX " (%s)\n",
        expected,
        expected_expression,
        actual,
        actual_expression
    );
    return false;
}

static inline void test_print_quoted(
    FILE *stream,
    const char *text,
    size_t length
) {
    fputc('"', stream);

    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)text[i];

        if (c == '"' || c == '\\') {
            fprintf(stream, "\\%c", c);
        } else if (c == '\n') {
            fputs("\\n", stream);
        } else if (c == '\t') {
            fputs("\\t", stream);
        } else if (c < 0x20 || c >= 0x7f) {
            fprintf(stream, "\\x%02x", c);
        } else {
            fputc(c, stream);
        }
    }

    fputc('"', stream);
}

static inline bool test_check_mem(
    const char *expected,
    size_t expected_length,
    const char *actual,
    size_t actual_length,
    const char *expected_expression,
    const char *actual_expression,
    const char *file,
    int line
) {
    if (expected != NULL && actual != NULL &&
        expected_length == actual_length &&
        memcmp(expected, actual, actual_length) == 0) {
        return true;
    }

    if (expected == NULL && actual == NULL) {
        return true;
    }

    test_report_failure_header(file, line);
    fputs("    expected: ", stderr);

    if (expected == NULL) {
        fputs("NULL", stderr);
    } else {
        test_print_quoted(stderr, expected, expected_length);
    }

    fprintf(stderr, " (%s)\n    actual:   ", expected_expression);

    if (actual == NULL) {
        fputs("NULL", stderr);
    } else {
        test_print_quoted(stderr, actual, actual_length);
    }

    fprintf(stderr, " (%s)\n", actual_expression);
    return false;
}

/* Like test_check_mem() with a NUL-terminated expectation; evaluates
 * 'expected' only once. */
static inline bool test_check_mem_cstr(
    const char *expected,
    const char *actual,
    size_t actual_length,
    const char *expected_expression,
    const char *actual_expression,
    const char *file,
    int line
) {
    return test_check_mem(
        expected,
        expected != NULL ? strlen(expected) : 0,
        actual,
        actual_length,
        expected_expression,
        actual_expression,
        file,
        line
    );
}

static inline bool test_check_str(
    const char *expected,
    const char *actual,
    const char *expected_expression,
    const char *actual_expression,
    const char *file,
    int line
) {
    return test_check_mem(
        expected,
        expected != NULL ? strlen(expected) : 0,
        actual,
        actual != NULL ? strlen(actual) : 0,
        expected_expression,
        actual_expression,
        file,
        line
    );
}

/* Prints the summary; returns the process exit status. */
static inline int test_finish(void) {
    printf(
        "\n%zu tests run, %zu passed, %zu failed",
        test_state.tests_run,
        test_state.tests_run - test_state.tests_failed,
        test_state.tests_failed
    );

    if (test_state.tests_skipped != 0) {
        printf(", %zu skipped by filter", test_state.tests_skipped);
    }

    printf(" (%zu failed checks)\n", test_state.checks_failed);

    if (test_state.tests_failed != 0 || test_state.tests_run == 0) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#define EXPECT_TRUE(condition) \
    ((void)test_check_true((condition) ? true : false, #condition, \
                           __FILE__, __LINE__))

#define EXPECT_FALSE(condition) \
    ((void)test_check_true((condition) ? false : true, "!(" #condition ")", \
                           __FILE__, __LINE__))

/*
 * 1 when 'value' has an unsigned integer type (size_t, uint64_t, and enums
 * the compiler represents as unsigned int). The operand of _Generic is not
 * evaluated.
 */
#define TEST_IS_UNSIGNED(value) \
    _Generic((value), \
        unsigned char: 1, \
        unsigned short: 1, \
        unsigned int: 1, \
        unsigned long: 1, \
        unsigned long long: 1, \
        default: 0)

/* Integer equality. Compared as uintmax_t when 'actual' is unsigned (so
 * values above INTMAX_MAX print correctly), otherwise as intmax_t. Each
 * operand is evaluated exactly once. */
#define TEST_CHECK_EQ(expected, actual, expected_text, actual_text) \
    (TEST_IS_UNSIGNED(actual) \
        ? test_check_uint((uintmax_t)(expected), (uintmax_t)(actual), \
                          (expected_text), (actual_text), __FILE__, __LINE__) \
        : test_check_int((intmax_t)(expected), (intmax_t)(actual), \
                         (expected_text), (actual_text), __FILE__, __LINE__))

#define EXPECT_EQ(expected, actual) \
    ((void)TEST_CHECK_EQ(expected, actual, #expected, #actual))

#define EXPECT_STR_EQ(expected, actual) \
    ((void)test_check_str((expected), (actual), #expected, #actual, \
                          __FILE__, __LINE__))

/* Compares a NUL-terminated expectation with a (pointer, length) slice. */
#define EXPECT_MEM_EQ(expected, actual, actual_length) \
    ((void)test_check_mem_cstr((expected), (actual), (actual_length), \
                               #expected, #actual, __FILE__, __LINE__))

#define ASSERT_TRUE(condition) \
    do { \
        if (!test_check_true((condition) ? true : false, #condition, \
                             __FILE__, __LINE__)) { \
            return; \
        } \
    } while (0)

#define ASSERT_EQ(expected, actual) \
    do { \
        if (!TEST_CHECK_EQ(expected, actual, #expected, #actual)) { \
            return; \
        } \
    } while (0)

#endif
