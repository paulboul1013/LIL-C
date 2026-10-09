#include "lil.h"
#include "sema.h"
#include "symbol.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Unit tests for stage 3: the symbol table and semantic analysis. Each
 * test names the docs/spec.md clause it checks.
 */

/* Runs the whole front end on a NUL-terminated program. */
static bool analyze(const char *text, Lil_Result *result) {
    return lil_analyze_source("sema.lil", text, strlen(text), result);
}

/* The first statement's expression. */
static Expr *first_expression(const Lil_Result *result, size_t index) {
    if (index >= result->program.statement_count ||
        result->program.statements[index]->kind != STMT_EXPRESSION) {
        return NULL;
    }

    return result->program.statements[index]->as.expression;
}

/* Expects exactly one error (stage semantic) at offset with message. */
static void expect_single_error(
    const char *text,
    size_t offset,
    const char *message
) {
    Lil_Result result;

    EXPECT_FALSE(analyze(text, &result));
    EXPECT_FALSE(result.has_program);
    EXPECT_FALSE(result.is_analyzed);
    EXPECT_EQ(1, result.errors.count);
    EXPECT_EQ(DIAGNOSTIC_SEMANTIC, result.diagnostic.stage);
    EXPECT_EQ(DIAGNOSTIC_ERROR, result.diagnostic.severity);
    EXPECT_EQ(offset, result.diagnostic.span.offset);
    EXPECT_STR_EQ(message, result.diagnostic.message);
    lil_result_free(&result);
}

/* Expects success with exactly the given warnings (offsets, messages). */
static void expect_warnings(
    const char *text,
    size_t count,
    const size_t *offsets,
    const char *const *messages
) {
    Lil_Result result;

    EXPECT_TRUE(analyze(text, &result));
    EXPECT_TRUE(result.is_analyzed);
    EXPECT_EQ(0, result.errors.count);
    EXPECT_EQ(count, result.warnings.count);

    for (size_t i = 0; i < count && i < result.warnings.count; ++i) {
        const Diagnostic *warning = &result.warnings.items[i];

        EXPECT_EQ(DIAGNOSTIC_WARNING, warning->severity);
        EXPECT_EQ(DIAGNOSTIC_SEMANTIC, warning->stage);
        EXPECT_TRUE(diagnostic_is_warning(warning));
        EXPECT_FALSE(diagnostic_has_error(warning));
        EXPECT_EQ(offsets[i], warning->span.offset);
        EXPECT_STR_EQ(messages[i], warning->message);
    }

    lil_result_free(&result);
}

static void expect_no_warnings(const char *text) {
    expect_warnings(text, 0, NULL, NULL);
}

/* ---- symbol table ---------------------------------------------------- */

/* §5.2 names are case-insensitive; ids are dense in first-seen order. */
TEST(symbol_table_folds_case_and_numbers_densely) {
    Symbol_Table table;
    symbol_table_init(&table);

    size_t a = SYMBOL_NONE;
    size_t b = SYMBOL_NONE;
    size_t c = SYMBOL_NONE;
    const Source_Span span = {3, 2, 1, 4};

    EXPECT_EQ(SYMBOL_NONE, symbol_table_find(&table, "x", 1));
    EXPECT_TRUE(symbol_table_intern(&table, "Ab", 2, span, &a));
    EXPECT_TRUE(symbol_table_intern(&table, "r0", 2, span, &b));
    EXPECT_TRUE(symbol_table_intern(&table, "aB", 2, span, &c));
    EXPECT_EQ(0, a);
    EXPECT_EQ(1, b);
    EXPECT_EQ(0, c);
    EXPECT_EQ(2, table.count);
    EXPECT_STR_EQ("ab", table.symbols[0].name);
    EXPECT_EQ(2, table.symbols[0].name_length);
    EXPECT_EQ(3, table.symbols[0].first_span.offset);
    EXPECT_EQ(1, symbol_table_find(&table, "R0", 2));
    EXPECT_EQ(SYMBOL_NONE, symbol_table_find(&table, "r", 1));
    EXPECT_EQ(SYMBOL_NONE, symbol_table_find(&table, "r00", 3));

    symbol_table_free(&table);
    symbol_table_free(&table);
    EXPECT_EQ(0, table.count);
    EXPECT_TRUE(table.symbols == NULL);
}

/* §5.2 the comparison is ASCII-only case folding of whole names. */
TEST(symbol_name_equals_compares_whole_names) {
    EXPECT_TRUE(symbol_name_equals("PuTc", 4, "putc"));
    EXPECT_TRUE(symbol_name_equals("", 0, ""));
    EXPECT_FALSE(symbol_name_equals("put", 3, "putc"));
    EXPECT_FALSE(symbol_name_equals("putcc", 5, "putc"));
    EXPECT_FALSE(symbol_name_equals("putd", 4, "putc"));
    EXPECT_TRUE(symbol_name_equals("_.9", 3, "_.9"));
    EXPECT_FALSE(symbol_name_equals("[", 1, "{"));   /* not 'Z' + 1 */
}

/* §5.1 many variables: the table grows and every name keeps its id. */
TEST(symbol_table_grows_with_many_names) {
    Symbol_Table table;
    symbol_table_init(&table);

    char name[32];
    const Source_Span span = {0, 1, 1, 1};

    for (size_t i = 0; i < 5000; ++i) {
        size_t id = SYMBOL_NONE;
        const int length = snprintf(name, sizeof(name), "V%zu", i);

        EXPECT_TRUE(symbol_table_intern(&table, name, (size_t)length, span,
                                        &id));
        EXPECT_EQ(i, id);
    }

    EXPECT_EQ(5000, table.count);

    for (size_t i = 0; i < 5000; i += 7) {
        const int length = snprintf(name, sizeof(name), "v%zu", i);
        EXPECT_EQ(i, symbol_table_find(&table, name, (size_t)length));
    }

    symbol_table_free(&table);
}

/* ---- resolution ------------------------------------------------------ */

/* §5.2 X and x are the same variable; every reference carries its id. */
TEST(sema_resolves_names_case_insensitively) {
    Lil_Result result;

    ASSERT_TRUE(analyze("X = 1; x = x + 1; Y = X; r0 = y;", &result));
    EXPECT_TRUE(result.is_analyzed);
    EXPECT_EQ(3, result.symbols.count);
    EXPECT_STR_EQ("x", result.symbols.symbols[0].name);
    EXPECT_STR_EQ("y", result.symbols.symbols[1].name);
    EXPECT_STR_EQ("r0", result.symbols.symbols[2].name);
    EXPECT_EQ(2, result.symbols.symbols[0].write_count);
    EXPECT_EQ(2, result.symbols.symbols[0].read_count);
    EXPECT_EQ(0, result.symbols.symbols[0].first_span.offset);
    EXPECT_EQ(0, result.symbols.symbols[0].first_write_span.offset);
    EXPECT_EQ(2, sema_exit_symbol(&result.symbols));

    const Expr *second = first_expression(&result, 1);

    if (second != NULL) {
        EXPECT_EQ(EXPR_ASSIGNMENT, second->kind);
        EXPECT_EQ(0, second->symbol);
        EXPECT_EQ(0, second->as.assignment.value->as.binary.left->symbol);
    }

    const Expr *third = first_expression(&result, 2);

    if (third != NULL) {
        EXPECT_EQ(1, third->symbol);
        EXPECT_EQ(0, third->as.assignment.value->symbol);
    }

    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    lil_result_free(&result);
}

/* §2.5, §5.2 IF and If are still keywords, not variables. */
TEST(sema_keywords_in_any_case_are_not_variables) {
    Lil_Result result;

    ASSERT_TRUE(analyze("IF (x) r0 = 1; If (x) r0 = 2; ELSE r0 = 3;",
                        &result));
    EXPECT_EQ(2, result.program.statement_count);
    EXPECT_EQ(STMT_IF, result.program.statements[0]->kind);
    EXPECT_EQ(STMT_IF, result.program.statements[1]->kind);
    EXPECT_EQ(2, result.symbols.count);
    EXPECT_EQ(SYMBOL_NONE, symbol_table_find(&result.symbols, "if", 2));
    lil_result_free(&result);
}

/* §5.5 a program that never mentions r0 has no exit symbol. */
TEST(sema_exit_symbol_is_none_without_r0) {
    Lil_Result result;

    ASSERT_TRUE(analyze("putc(1);", &result));
    EXPECT_EQ(0, result.symbols.count);
    EXPECT_EQ(SYMBOL_NONE, sema_exit_symbol(&result.symbols));
    lil_result_free(&result);

    ASSERT_TRUE(analyze("R0 = 3;", &result));
    EXPECT_EQ(0, sema_exit_symbol(&result.symbols));
    lil_result_free(&result);
}

/* §4.10 putc calls (any case) are marked as the builtin. */
TEST(sema_marks_putc_as_builtin) {
    Lil_Result result;

    ASSERT_TRUE(analyze("PUTC(putc('a') + 1);", &result));

    const Expr *call = first_expression(&result, 0);

    if (call != NULL) {
        EXPECT_EQ(EXPR_CALL, call->kind);
        EXPECT_EQ(BUILTIN_PUTC, call->builtin);
        EXPECT_EQ(BUILTIN_PUTC,
                  call->as.call.arguments[0]->as.binary.left->builtin);
    }

    EXPECT_EQ(0, result.symbols.count);
    lil_result_free(&result);
}

/* ---- errors ---------------------------------------------------------- */

/* §3.7 break/continue may appear at any depth of if or block in a loop. */
TEST(sema_accepts_break_and_continue_inside_loops) {
    const char *programs[] = {
        "while (x) if (y) break;",
        "while (x) { if (y) { break; } else continue; }",
        "while (x) { { { break; } } }",
        "do { if (x) continue; } while (y);",
        "while (a) { while (b) break; break; }",
        "do break; while (0);",
        "do { break; } while (x);",
    };

    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i) {
        Lil_Result result;

        EXPECT_TRUE(analyze(programs[i], &result));
        EXPECT_EQ(0, result.errors.count);
        lil_result_free(&result);
    }
}

/* §3.7 break/continue outside of every loop. */
TEST(sema_rejects_break_and_continue_outside_loops) {
    expect_single_error("break;", 0, "'break' outside of a loop");
    expect_single_error("continue;", 0, "'continue' outside of a loop");
    expect_single_error("if (x) { BREAK; }", 9, "'break' outside of a loop");
    expect_single_error("while (x) ; continue;", 12,
                        "'continue' outside of a loop");
    /* Without 'do', a block followed by 'while' is two statements: the
     * block is not the loop's body. */
    expect_single_error("{ break; } while (x) ;", 2,
                        "'break' outside of a loop");
    /* The loop that follows a do loop is not inside it either. */
    expect_single_error("do x = 1; while (y); break;", 21,
                        "'break' outside of a loop");
}

/* §4.10 putc takes exactly one argument. */
TEST(sema_checks_putc_argument_count) {
    expect_single_error("putc();", 0, "'putc' expects 1 argument, got 0");
    expect_single_error("x = 1 + PutC(1, 2);", 8,
                        "'putc' expects 1 argument, got 2");
}

/* §4.10 only putc can be called. */
TEST(sema_rejects_unknown_functions) {
    expect_single_error("f(1);", 0, "unknown function 'f'");
    expect_single_error("r0 = Foo();", 5, "unknown function 'Foo'");
}

/* §5.4 putc is neither read nor assigned as a variable. */
TEST(sema_rejects_putc_as_variable) {
    const char *message =
        "'putc' is a builtin function and cannot be used as a variable";

    expect_single_error("putc = 1;", 0, message);
    expect_single_error("x = PUTC = 1;", 4, message);
    expect_single_error("r0 = putc + 1;", 5, message);
    expect_single_error("putc(putc);", 5, message);
    /* The erroneous statement does not also get "no effect". */
    expect_single_error("putc;", 0, message);
}

/* §6.3.4 messages keep long names readable and bounded. */
TEST(sema_shortens_long_names_in_messages) {
    char text[400];
    memset(text, 'a', 300);
    memcpy(text + 300, "();", 4);

    Lil_Result result;

    EXPECT_FALSE(analyze(text, &result));
    EXPECT_EQ(1, result.errors.count);
    EXPECT_EQ(18 + 200 + 4, strlen(result.diagnostic.message));
    EXPECT_TRUE(strstr(result.diagnostic.message, "a...'") != NULL);
    lil_result_free(&result);

    memcpy(text + 300, " = 1;", 6);
    EXPECT_TRUE(analyze(text, &result));
    EXPECT_EQ(1, result.warnings.count);
    EXPECT_TRUE(strstr(result.warnings.items[0].message,
                       "a...' is assigned but never read") != NULL);
    lil_result_free(&result);
}

/* §6.1 several semantic errors are reported in source order. */
TEST(sema_reports_every_error_in_source_order) {
    Lil_Result result;

    EXPECT_FALSE(analyze("break;\nf(putc);\nputc(1, 2) = 3;", &result));
    /* "putc(1, 2) = 3" is a syntax error; sema never runs. */
    EXPECT_EQ(DIAGNOSTIC_SYNTAX, result.diagnostic.stage);
    lil_result_free(&result);

    EXPECT_FALSE(analyze("break;\nf(putc);\nx = putc(1, 2);\ncontinue;",
                         &result));
    ASSERT_TRUE(result.errors.count == 5);
    const size_t offsets[] = {0, 7, 9, 20, 32};

    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(offsets[i], result.errors.items[i].span.offset);
        EXPECT_EQ(DIAGNOSTIC_SEMANTIC, result.errors.items[i].stage);
    }

    EXPECT_EQ(0, result.diagnostic.span.offset);
    EXPECT_STR_EQ("unknown function 'f'", result.errors.items[1].message);
    lil_result_free(&result);
}

/* §6.3.4 at most 20 errors; the incomplete walk adds no unused warnings. */
TEST(sema_caps_error_count) {
    char text[512] = "x = 1; 1;";

    for (size_t i = 0; i < 25; ++i) {
        strcat(text, " break;");
    }

    Lil_Result result;

    EXPECT_FALSE(analyze(text, &result));
    EXPECT_EQ(DIAGNOSTIC_MAX_ERRORS, result.errors.count);
    EXPECT_EQ(1, result.warnings.count);
    EXPECT_STR_EQ("statement has no effect",
                  result.warnings.items[0].message);
    lil_result_free(&result);
}

/* §6.3.4 the cap can be reached in the middle of one expression: the
 * rest of it is skipped and the 20 errors stay in source order. */
TEST(sema_caps_error_count_inside_an_expression) {
    char text[1024] = "x = 1; putc(";

    for (size_t i = 0; i < 30; ++i) {
        char argument[32];
        snprintf(argument, sizeof(argument), "%sputc + f%zu()",
                 i == 0 ? "" : ", ", i);
        strcat(text, argument);
    }

    strcat(text, "); break;");

    Lil_Result result;

    EXPECT_FALSE(analyze(text, &result));
    ASSERT_TRUE(result.errors.count == DIAGNOSTIC_MAX_ERRORS);
    EXPECT_STR_EQ("'putc' expects 1 argument, got 30",
                  result.errors.items[0].message);
    EXPECT_STR_EQ("'putc' is a builtin function and cannot be used as a "
                  "variable",
                  result.errors.items[1].message);
    EXPECT_STR_EQ("unknown function 'f0'", result.errors.items[2].message);
    /* 1 + 2 per argument: the 20th error is f8's putc. */
    EXPECT_STR_EQ("'putc' is a builtin function and cannot be used as a "
                  "variable",
                  result.errors.items[19].message);

    for (size_t i = 1; i < result.errors.count; ++i) {
        EXPECT_EQ(DIAGNOSTIC_SEMANTIC, result.errors.items[i].stage);
        EXPECT_TRUE(result.errors.items[i].span.offset >
                    result.errors.items[i - 1].span.offset);
    }

    /* Stopped: no unused-variable warning for x. */
    EXPECT_EQ(0, result.warnings.count);
    lil_result_free(&result);
    /* Reached in an if condition: the branch is not analyzed at all. */
    char condition[512] = "if (f0()";

    for (size_t i = 1; i < 20; ++i) {
        char term[16];
        snprintf(term, sizeof(term), " + f%zu()", i);
        strcat(condition, term);
    }

    strcat(condition, ") break;");
    EXPECT_FALSE(analyze(condition, &result));
    EXPECT_EQ(DIAGNOSTIC_MAX_ERRORS, result.errors.count);
    EXPECT_STR_EQ("unknown function 'f19'",
                  result.errors.items[DIAGNOSTIC_MAX_ERRORS - 1].message);
    lil_result_free(&result);
}

/* ---- warnings -------------------------------------------------------- */

/* §3.2, §6.2 "r0 - 1;" (the old basic.lil) has no effect. */
TEST(sema_warns_about_statements_without_effect) {
    const size_t offsets[] = {10, 18, 24, 26, 30};
    const char *const messages[] = {
        "statement has no effect", "statement has no effect",
        "statement has no effect", "statement has no effect",
        "statement has no effect",
    };

    expect_warnings("r0 = 1;\n  r0 - 1;\n( x );1;-~x;x && y || 'c' < 2;",
                    5, offsets, messages);
}

/* §3.2 assignments, calls and divisions (which may trap) have effects. */
TEST(sema_effects_suppress_the_no_effect_warning) {
    expect_no_warnings("r0 = 1;");
    expect_no_warnings("putc(r0);");
    expect_no_warnings("r0 / 0;");
    expect_no_warnings("-(r0 / 2) + 1;");
    expect_no_warnings("(r0 = 2) + 1;");
    expect_no_warnings("r0 || putc(1);");
    expect_no_warnings(";");
    expect_no_warnings("{ } while (r0) ; if (r0) ;");
}

/* §6.2 a variable that is assigned but never read; r0 is exempt (§5.5). */
TEST(sema_warns_about_variables_never_read) {
    const size_t offsets[] = {7, 20};
    const char *const messages[] = {
        "variable 'Y' is assigned but never read",
        "variable 'zz' is assigned but never read",
    };

    expect_warnings("x = 1; Y = x; y = 2;zz = 3; R0 = 4; r0 = 5;",
                    2, offsets, messages);
    /* Read before it is assigned still counts as read. */
    expect_no_warnings("r0 = a; a = 1;");
    /* A variable that is only read is not reported. */
    expect_no_warnings("r0 = never_assigned;");
    /* Reading in its own assignment counts. */
    expect_no_warnings("n = n + 1;");
}

/* §6.2 never-read variables are reported in source order without
 * sorting: their ids follow their first writes (include/sema.h, 5). */
TEST(sema_unused_variables_follow_id_order) {
    const size_t offsets[] = {0, 21};
    const char *const messages[] = {
        "variable 'b' is assigned but never read",
        "variable 'd' is assigned but never read",
    };
    const char *text = "b = 1; a = 2; c = a; d = c;";

    expect_warnings(text, 2, offsets, messages);

    Lil_Result result;

    ASSERT_TRUE(analyze(text, &result));
    EXPECT_EQ(4, result.symbols.count);

    for (size_t i = 1; i < result.symbols.count; ++i) {
        EXPECT_TRUE(result.symbols.symbols[i].first_span.offset >
                    result.symbols.symbols[i - 1].first_span.offset);
    }

    EXPECT_STR_EQ("d", result.symbols.symbols[3].name);
    EXPECT_EQ(result.symbols.symbols[3].first_span.offset,
              result.symbols.symbols[3].first_write_span.offset);
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    lil_result_free(&result);
}

/* §6.2 both kinds of warnings are merged in source order. */
TEST(sema_merges_warnings_in_source_order) {
    const size_t offsets[] = {0, 6, 13, 19};
    const char *const messages[] = {
        "statement has no effect",
        "variable 'b' is assigned but never read",
        "variable 'c' is assigned but never read",
        "statement has no effect",
    };

    expect_warnings("a + 1;b = a; c = 2;3;", 4, offsets, messages);
}

/* §6.2 at most 20 warnings, the first ones in source order. */
TEST(sema_caps_warning_count) {
    char text[1024] = "";

    for (size_t i = 0; i < 30; ++i) {
        char statement[16];
        snprintf(statement, sizeof(statement), "v%zu = 1; ", i);
        strcat(text, statement);
        strcat(text, "1; ");
    }

    Lil_Result result;

    ASSERT_TRUE(analyze(text, &result));
    EXPECT_EQ(DIAGNOSTIC_MAX_WARNINGS, result.warnings.count);

    for (size_t i = 1; i < result.warnings.count; ++i) {
        EXPECT_TRUE(result.warnings.items[i].span.offset >
                    result.warnings.items[i - 1].span.offset);
    }

    EXPECT_STR_EQ("variable 'v0' is assigned but never read",
                  result.warnings.items[0].message);
    EXPECT_STR_EQ("statement has no effect",
                  result.warnings.items[19].message);
    lil_result_free(&result);
}

/* ---- the front-end seam ---------------------------------------------- */

/* §6.1, §6.2 the merged listing prints errors and warnings in order. */
TEST(result_prints_errors_and_warnings_merged) {
    Lil_Result result;
    FILE *stream = tmpfile();

    ASSERT_TRUE(stream != NULL);
    EXPECT_FALSE(analyze("1;\nbreak;\nx = 2;\nf();\n", &result));
    lil_result_fprint_diagnostics(stream, &result);
    lil_result_free(&result);

    char buffer[512] = "";
    rewind(stream);
    const size_t length = fread(buffer, 1, sizeof(buffer) - 1, stream);
    buffer[length] = '\0';
    fclose(stream);

    EXPECT_STR_EQ("sema.lil:1:1: warning: statement has no effect\n"
                  "sema.lil:2:1: semantic error: 'break' outside of a loop\n"
                  "sema.lil:3:1: warning: variable 'x' is assigned but "
                  "never read\n"
                  "sema.lil:4:1: semantic error: unknown function 'f'\n",
                  buffer);
}

/* §6.1 every stage prints its name; a plain list prints in order. */
TEST(diagnostic_list_prints_stage_names) {
    Source source;
    ASSERT_TRUE(source_init(&source, "d.lil", "x", 1));

    Diagnostic_List list;
    Diagnostic diagnostic;
    const Source_Span span = {0, 1, 1, 1};
    const Diagnostic_Stage stages[] = {
        DIAGNOSTIC_INTERNAL, DIAGNOSTIC_SEMANTIC, DIAGNOSTIC_NONE,
    };

    diagnostic_list_clear(&list);

    for (size_t i = 0; i < 3; ++i) {
        diagnostic_clear(&diagnostic);
        diagnostic_set(&diagnostic, stages[i], span, "m%zu", i);
        diagnostic_list_add(&list, &diagnostic);
    }

    EXPECT_FALSE(diagnostic_is_warning(&list.items[0]));
    EXPECT_FALSE(diagnostic_has_error(&list.items[2]));

    FILE *stream = tmpfile();

    if (stream != NULL) {
        diagnostic_list_fprint(stream, &source, &list);

        char buffer[256] = "";
        rewind(stream);
        buffer[fread(buffer, 1, sizeof(buffer) - 1, stream)] = '\0';
        fclose(stream);

        EXPECT_STR_EQ("d.lil:1:1: internal error: m0\n"
                      "d.lil:1:1: semantic error: m1\n"
                      "d.lil:1:1: unknown error: m2\n",
                      buffer);
    }

    source_free(&source);
}

/* Syntax errors stop the front end before semantic analysis. */
TEST(analyze_reports_syntax_errors_without_semantic_ones) {
    Lil_Result result;

    EXPECT_FALSE(analyze("break; x = ;", &result));
    EXPECT_EQ(1, result.errors.count);
    EXPECT_EQ(DIAGNOSTIC_SYNTAX, result.diagnostic.stage);
    EXPECT_EQ(0, result.warnings.count);
    EXPECT_EQ(0, result.symbols.count);
    lil_result_free(&result);
}

/* lil_run_source() stays syntactic: it accepts what sema rejects. */
TEST(run_source_does_not_analyze) {
    Lil_Result result;

    EXPECT_TRUE(lil_run_source("p.lil", "f(); break;", 11, &result));
    EXPECT_TRUE(result.has_program);
    EXPECT_FALSE(result.is_analyzed);
    lil_result_free(&result);
}

/* include/ast.h: the parser leaves names unresolved (SYMBOL_NONE, never a
 * valid id), so an unanalyzed tree does not pass sema_verify(). */
TEST(parser_leaves_symbols_unresolved) {
    const char *text = "x = y;";
    Lil_Result result;

    ASSERT_TRUE(lil_run_source("p.lil", text, strlen(text), &result));

    const Expr *assignment = first_expression(&result, 0);

    ASSERT_TRUE(assignment != NULL);
    EXPECT_EQ(SYMBOL_NONE, assignment->symbol);
    EXPECT_EQ(SYMBOL_NONE, assignment->as.assignment.value->symbol);
    EXPECT_EQ(BUILTIN_NONE, assignment->builtin);
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    lil_result_free(&result);
}

/* source_text_at() returns "" for a span outside the source. */
TEST(source_text_at_rejects_out_of_range_spans) {
    Source source;

    ASSERT_TRUE(source_init(&source, "t.lil", "abc", 3));
    EXPECT_STR_EQ("bc", source_text_at(&source, (Source_Span){1, 2, 1, 2}));
    EXPECT_STR_EQ("", source_text_at(&source, (Source_Span){3, 0, 1, 4}));
    EXPECT_STR_EQ("", source_text_at(&source, (Source_Span){4, 0, 1, 5}));
    EXPECT_STR_EQ("", source_text_at(&source, (Source_Span){2, 2, 1, 3}));
    EXPECT_STR_EQ("", source_text_at(&source,
                                     (Source_Span){1, SIZE_MAX, 1, 2}));
    source_free(&source);
}

/* §2.1 the end-of-source span (where a late internal error is placed)
 * sorts after every token. */
TEST(source_end_span_follows_the_last_byte) {
    Source source;

    ASSERT_TRUE(source_init(&source, "e.lil", "ab\n\tc", 5));
    Source_Span span = source_end_span(&source);
    EXPECT_EQ(5, span.offset);
    EXPECT_EQ(0, span.length);
    EXPECT_EQ(2, span.line);
    EXPECT_EQ(3, span.column);
    source_free(&source);

    ASSERT_TRUE(source_init(&source, "e.lil", "", 0));
    span = source_end_span(&source);
    EXPECT_EQ(0, span.offset);
    EXPECT_EQ(1, span.line);
    EXPECT_EQ(1, span.column);
    source_free(&source);

    ASSERT_TRUE(source_init(&source, "e.lil", "x;\n", 3));
    span = source_end_span(&source);
    EXPECT_EQ(3, span.offset);
    EXPECT_EQ(2, span.line);
    EXPECT_EQ(1, span.column);
    source_free(&source);
}

/* §6.3 lil_result_free() is idempotent and clears the new fields. */
TEST(analyze_result_free_is_repeatable) {
    Lil_Result result;

    EXPECT_TRUE(analyze("x = 1; 2;", &result));
    EXPECT_EQ(2, result.warnings.count);
    lil_result_free(&result);
    lil_result_free(&result);
    EXPECT_FALSE(result.is_analyzed);
    EXPECT_FALSE(result.has_program);
    EXPECT_EQ(0, result.warnings.count);
    EXPECT_EQ(0, result.symbols.count);
    EXPECT_TRUE(result.symbols.symbols == NULL);

    EXPECT_FALSE(analyze("break;", &result));
    lil_result_free(&result);
    lil_result_free(&result);
    EXPECT_EQ(0, result.errors.count);
}

/* ---- sema_verify ----------------------------------------------------- */

/* include/sema.h: sema_verify() rejects ASTs that break an invariant. */
TEST(sema_verify_detects_broken_invariants) {
    Lil_Result result;

    ASSERT_TRUE(analyze("a = b; while (a) { if (b) break; else continue; }"
                        " do putc(a); while (0); r0 = -a; ;",
                        &result));
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));

    Stmt **statements = result.program.statements;
    Expr *assignment = statements[0]->as.expression;
    Expr *value = assignment->as.assignment.value;

    /* 1: a symbol id out of range, or naming another variable. */
    value->symbol = result.symbols.count;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    value->symbol = assignment->symbol;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    value->symbol = 1;

    assignment->symbol = 1;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    assignment->symbol = 0;

    /* 3: an unresolved call. */
    Stmt *do_loop = statements[2];
    Expr *call = do_loop->as.do_statement.body->as.expression;
    call->builtin = BUILTIN_NONE;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    call->builtin = BUILTIN_PUTC;

    /* 4: a break that is no longer inside a loop. */
    Stmt *loop = statements[1];
    loop->kind = STMT_IF;
    loop->as.if_statement.else_branch = NULL;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    loop->kind = STMT_WHILE;

    /* 6: a missing child. */
    Expr *saved = do_loop->as.do_statement.condition;
    do_loop->as.do_statement.condition = NULL;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    do_loop->as.do_statement.condition = saved;

    Expr *negated = statements[3]->as.expression->as.assignment.value;
    Expr *operand = negated->as.unary.operand;
    negated->as.unary.operand = NULL;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    negated->as.unary.operand = operand;

    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));

    /* 2: a symbol named putc. */
    size_t id = SYMBOL_NONE;
    EXPECT_TRUE(symbol_table_intern(&result.symbols, "PUTC", 4,
                                    result.program.span, &id));
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    lil_result_free(&result);
}

/* include/sema.h 1, 5, 6: sema_verify() also checks the symbol table and
 * the node contents that lowering switches on. */
TEST(sema_verify_checks_symbols_operators_and_literals) {
    Lil_Result result;

    ASSERT_TRUE(analyze("a = 1 + 'c'; b = -a; r0 = b * 07;", &result));
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));

    Stmt **statements = result.program.statements;
    Expr *first = statements[0]->as.expression;
    Expr *sum = first->as.assignment.value;
    Expr *negation = statements[1]->as.expression->as.assignment.value;
    Expr *product = statements[2]->as.expression->as.assignment.value;
    Symbol *symbols = result.symbols.symbols;

    /* 1: an unresolved name. */
    first->symbol = SYMBOL_NONE;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    first->symbol = 0;

    /* 6: the name of an assignment or variable is an identifier. */
    first->as.assignment.name.kind = TOKEN_NUMBER;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    first->as.assignment.name.kind = TOKEN_IDENTIFIER;

    /* 6: binary operators are §4.2 entries other than '='. */
    sum->as.binary.operator_token.kind = TOKEN_ASSIGN;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    sum->as.binary.operator_token.kind = TOKEN_TILDE;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    sum->as.binary.operator_token.kind = TOKEN_PLUS;

    /* 6: unary operators are '-' and '~'. */
    negation->as.unary.operator_token.kind = TOKEN_PLUS;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    negation->as.unary.operator_token.kind = TOKEN_TILDE;
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    negation->as.unary.operator_token.kind = TOKEN_MINUS;

    /* 6: literal kinds and ranges. */
    Token *number = &sum->as.binary.left->as.literal;
    Token *character = &sum->as.binary.right->as.literal;

    number->value = -1;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    number->value = INT64_MAX;
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    number->value = 1;
    character->value = 256;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    character->value = -1;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    character->value = 255;
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    character->kind = TOKEN_IDENTIFIER;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    character->kind = TOKEN_CHARACTER;
    EXPECT_EQ(7, product->as.binary.right->as.literal.value);

    /* 5: names are unique (each is found at its own id). */
    symbols[1].name[0] = 'a';
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    symbols[1].name[0] = 'b';

    /* 5: name_length matches the name. */
    symbols[1].name_length = 2;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    symbols[1].name_length = 1;

    /* 5: ids follow the first appearance. */
    const Source_Span first_b = symbols[1].first_span;
    symbols[1].first_span = symbols[0].first_span;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    symbols[1].first_span = first_b;

    /* 5: a never-read symbol first appears at its first write. */
    symbols[2].first_write_span.offset += 1;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    symbols[2].first_write_span.offset -= 1;

    /* Every symbol is mentioned. */
    const size_t reads = symbols[1].read_count;
    const size_t writes = symbols[1].write_count;
    symbols[1].read_count = 0;
    symbols[1].write_count = 0;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    symbols[1].read_count = reads;
    symbols[1].write_count = writes;

    /* 6: required children, including a block's statement array. */
    Stmt **saved = result.program.statements;
    result.program.statements = NULL;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    result.program.statements = saved;

    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    lil_result_free(&result);

    ASSERT_TRUE(analyze("{ putc(1); } if (1) ; else { }", &result));
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));

    Stmt *block = result.program.statements[0];
    Stmt **inner = block->as.block.statements;
    block->as.block.statements = NULL;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    block->as.block.statements = inner;

    Expr *call = inner[0]->as.expression;
    Expr **arguments = call->as.call.arguments;
    call->as.call.arguments = NULL;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    call->as.call.arguments = arguments;

    /* else_branch is the only child that may be NULL. */
    Stmt *if_statement = result.program.statements[1];
    Stmt *else_branch = if_statement->as.if_statement.else_branch;
    if_statement->as.if_statement.else_branch = NULL;
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    if_statement->as.if_statement.else_branch = else_branch;

    Stmt *then_branch = if_statement->as.if_statement.then_branch;
    if_statement->as.if_statement.then_branch = NULL;
    EXPECT_FALSE(sema_verify(&result.program, &result.source,
                             &result.symbols));
    if_statement->as.if_statement.then_branch = then_branch;

    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    lil_result_free(&result);
}

/* §6.3.1 analysis handles the deepest trees the parser accepts. */
TEST(sema_handles_maximum_nesting) {
    const size_t depth = 255;
    const size_t size = depth * 16 + 64;
    char *text = malloc(size);

    ASSERT_TRUE(text != NULL);
    text[0] = '\0';

    for (size_t i = 0; i < depth; ++i) {
        strcat(text, "while (x) ");
    }

    strcat(text, "break;");

    Lil_Result result;

    EXPECT_TRUE(analyze(text, &result));
    EXPECT_TRUE(sema_verify(&result.program, &result.source,
                            &result.symbols));
    lil_result_free(&result);
    free(text);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        test_set_filter(argv[1]);
    }

    RUN_TEST(symbol_table_folds_case_and_numbers_densely);
    RUN_TEST(symbol_name_equals_compares_whole_names);
    RUN_TEST(symbol_table_grows_with_many_names);
    RUN_TEST(sema_resolves_names_case_insensitively);
    RUN_TEST(sema_keywords_in_any_case_are_not_variables);
    RUN_TEST(sema_exit_symbol_is_none_without_r0);
    RUN_TEST(sema_marks_putc_as_builtin);
    RUN_TEST(sema_accepts_break_and_continue_inside_loops);
    RUN_TEST(sema_rejects_break_and_continue_outside_loops);
    RUN_TEST(sema_checks_putc_argument_count);
    RUN_TEST(sema_rejects_unknown_functions);
    RUN_TEST(sema_rejects_putc_as_variable);
    RUN_TEST(sema_shortens_long_names_in_messages);
    RUN_TEST(sema_reports_every_error_in_source_order);
    RUN_TEST(sema_caps_error_count);
    RUN_TEST(sema_caps_error_count_inside_an_expression);
    RUN_TEST(sema_warns_about_statements_without_effect);
    RUN_TEST(sema_effects_suppress_the_no_effect_warning);
    RUN_TEST(sema_warns_about_variables_never_read);
    RUN_TEST(sema_unused_variables_follow_id_order);
    RUN_TEST(sema_merges_warnings_in_source_order);
    RUN_TEST(sema_caps_warning_count);
    RUN_TEST(result_prints_errors_and_warnings_merged);
    RUN_TEST(diagnostic_list_prints_stage_names);
    RUN_TEST(analyze_reports_syntax_errors_without_semantic_ones);
    RUN_TEST(run_source_does_not_analyze);
    RUN_TEST(parser_leaves_symbols_unresolved);
    RUN_TEST(source_text_at_rejects_out_of_range_spans);
    RUN_TEST(source_end_span_follows_the_last_byte);
    RUN_TEST(analyze_result_free_is_repeatable);
    RUN_TEST(sema_verify_detects_broken_invariants);
    RUN_TEST(sema_verify_checks_symbols_operators_and_literals);
    RUN_TEST(sema_handles_maximum_nesting);
    return test_finish();
}
