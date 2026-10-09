#include "ast_print.h"
#include "lexer.h"
#include "lil.h"
#include "operator.h"
#include "parser.h"
#include "source.h"
#include "test_framework.h"

#include <dirent.h>
#include <locale.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Unit tests for the public front-end seams. Each test names the
 * docs/spec.md clause it checks.
 */

#define EXPECT_TOKEN_TEXT(source, token, expected) \
    EXPECT_MEM_EQ( \
        (expected), \
        source_text_at((source), (token).span), \
        (token).span.length \
    )

/* §2.4 identifiers; tokens keep spans into an owned copy of the source. */
TEST(lexer_tokens_refer_to_owned_source_by_span) {
    char input[] = "name";
    Source source;

    ASSERT_TRUE(source_init(&source, "memory.lil", input, strlen(input)));
    input[0] = 'X';

    Lexer lexer;
    lexer_init(&lexer, &source);

    const Token token = lexer_next_token(&lexer);

    EXPECT_EQ(TOKEN_IDENTIFIER, token.kind);
    EXPECT_EQ(0, token.span.offset);
    EXPECT_EQ(4, token.span.length);
    EXPECT_EQ(1, token.span.line);
    EXPECT_EQ(1, token.span.column);
    EXPECT_TOKEN_TEXT(&source, token, "name");

    source_free(&source);
}

/* §2.8 invalid characters; §6.1 diagnostics carry stage and position. */
TEST(lexer_reports_structured_diagnostic) {
    Source source;
    ASSERT_TRUE(source_init(&source, "bad.lil", "\n @", 3));

    Lexer lexer;
    lexer_init(&lexer, &source);

    const Token token = lexer_next_token(&lexer);
    const Diagnostic *diagnostic = lexer_diagnostic(&lexer);

    EXPECT_EQ(TOKEN_INVALID, token.kind);
    EXPECT_EQ(DIAGNOSTIC_LEXICAL, diagnostic->stage);
    EXPECT_EQ(2, diagnostic->span.offset);
    EXPECT_EQ(2, diagnostic->span.line);
    EXPECT_EQ(2, diagnostic->span.column);
    EXPECT_STR_EQ("unexpected character '@'", diagnostic->message);

    source_free(&source);
}

/* §2.1 whitespace and §2.2 comments are ignored; lines/columns are 1-based. */
TEST(lexer_reports_end_position_after_ignored_input) {
    const char input[] = "\n  x\n\t+ \n";
    Source source;
    Lexer lexer;
    Token token;

    ASSERT_TRUE(source_init(&source, "memory.lil", input, strlen(input)));
    lexer_init(&lexer, &source);

    token = lexer_next_token(&lexer);
    EXPECT_EQ(TOKEN_IDENTIFIER, token.kind);
    EXPECT_EQ(2, token.span.line);
    EXPECT_EQ(3, token.span.column);

    token = lexer_next_token(&lexer);
    EXPECT_EQ(TOKEN_PLUS, token.kind);
    EXPECT_EQ(3, token.span.line);
    EXPECT_EQ(2, token.span.column);

    token = lexer_next_token(&lexer);
    EXPECT_EQ(TOKEN_END, token.kind);
    EXPECT_EQ(strlen(input), token.span.offset);
    EXPECT_EQ(0, token.span.length);
    EXPECT_EQ(4, token.span.line);
    EXPECT_EQ(1, token.span.column);

    source_free(&source);
}

/* The expression of statement 'index' when it is an expression
 * statement, otherwise NULL. */
static const Expr *statement_expression(const Program *program, size_t index) {
    if (index >= program->statement_count ||
        program->statements[index]->kind != STMT_EXPRESSION) {
        return NULL;
    }

    return program->statements[index]->as.expression;
}

/* Checks 'expression' is non-NULL and of 'kind'; the caller only looks
 * inside the node when this returns true. */
static bool expect_kind(const Expr *expression, Expr_Kind kind) {
    EXPECT_TRUE(expression != NULL);

    if (expression == NULL) {
        return false;
    }

    EXPECT_EQ(kind, expression->kind);
    return expression->kind == kind;
}

static bool expect_binary(const Expr *expression, Token_Kind operator_kind) {
    if (!expect_kind(expression, EXPR_BINARY)) {
        return false;
    }

    EXPECT_EQ(operator_kind, expression->as.binary.operator_token.kind);
    return true;
}

static bool expect_variable(
    const Source *source,
    const Expr *expression,
    const char *name
) {
    if (!expect_kind(expression, EXPR_VARIABLE)) {
        return false;
    }

    EXPECT_TOKEN_TEXT(source, expression->as.variable, name);
    return true;
}

/* §4.2 precedence: '*' binds tighter than '+'. */
TEST(parser_builds_precedence_ast_at_public_seam) {
    Source source;
    ASSERT_TRUE(source_init(&source, "memory.lil", "a + b * c;", 10));

    Lexer lexer;
    lexer_init(&lexer, &source);
    Parser *parser = parser_create(&lexer);
    EXPECT_TRUE(parser != NULL);

    if (parser == NULL) {
        source_free(&source);
        return;
    }

    Program program;
    EXPECT_TRUE(parser_parse_program(parser, &program));
    EXPECT_EQ(1, program.statement_count);

    const Expr *addition = statement_expression(&program, 0);

    if (expect_binary(addition, TOKEN_PLUS) &&
        expect_variable(&source, addition->as.binary.left, "a") &&
        expect_binary(addition->as.binary.right, TOKEN_STAR)) {
        EXPECT_EQ(0, addition->span.offset);
        EXPECT_EQ(9, addition->span.length);

        const Expr *multiplication = addition->as.binary.right;
        EXPECT_EQ(4, multiplication->span.offset);
        EXPECT_EQ(5, multiplication->span.length);
        expect_variable(&source, multiplication->as.binary.left, "b");
        expect_variable(&source, multiplication->as.binary.right, "c");
    }

    program_free(&program);
    parser_destroy(parser);
    source_free(&source);
}

/* §4.2 assignment is right-associative. */
TEST(parser_builds_right_associative_assignment) {
    Source source;
    ASSERT_TRUE(source_init(&source, "memory.lil", "a = b = 1;", 10));

    Lexer lexer;
    lexer_init(&lexer, &source);
    Parser *parser = parser_create(&lexer);
    EXPECT_TRUE(parser != NULL);

    if (parser == NULL) {
        source_free(&source);
        return;
    }

    Program program;
    EXPECT_TRUE(parser_parse_program(parser, &program));
    EXPECT_EQ(1, program.statement_count);

    const Expr *outer = statement_expression(&program, 0);

    if (expect_kind(outer, EXPR_ASSIGNMENT) &&
        expect_kind(outer->as.assignment.value, EXPR_ASSIGNMENT)) {
        EXPECT_EQ(0, outer->span.offset);
        EXPECT_EQ(9, outer->span.length);
        EXPECT_TOKEN_TEXT(&source, outer->as.assignment.name, "a");

        const Expr *inner = outer->as.assignment.value;
        EXPECT_EQ(4, inner->span.offset);
        EXPECT_EQ(5, inner->span.length);
        EXPECT_TOKEN_TEXT(&source, inner->as.assignment.name, "b");

        if (expect_kind(inner->as.assignment.value, EXPR_LITERAL)) {
            EXPECT_TOKEN_TEXT(
                &source,
                inner->as.assignment.value->as.literal,
                "1"
            );
        }
    }

    program_free(&program);
    parser_destroy(parser);
    source_free(&source);
}

/* §3.1 a program is a sequence of statements parsed to end of file. */
TEST(run_source_owns_complete_program) {
    Lil_Result result;

    EXPECT_TRUE(lil_run_source("memory.lil", "x + 1; y = 2;", 13, &result));
    EXPECT_TRUE(result.has_program);
    EXPECT_EQ(2, result.program.statement_count);

    const Expr *first = statement_expression(&result.program, 0);
    const Expr *second = statement_expression(&result.program, 1);

    if (expect_kind(first, EXPR_BINARY) &&
        expect_kind(second, EXPR_ASSIGNMENT)) {
        EXPECT_TOKEN_TEXT(&result.source, second->as.assignment.name, "y");
    }

    lil_result_free(&result);
}

/* §2.8, §6.1 */
TEST(run_source_returns_lexical_diagnostic) {
    Lil_Result result;

    EXPECT_FALSE(lil_run_source("bad.lil", "@", 1, &result));
    EXPECT_FALSE(result.has_program);
    EXPECT_EQ(DIAGNOSTIC_LEXICAL, result.diagnostic.stage);
    EXPECT_EQ(1, result.diagnostic.span.line);
    EXPECT_EQ(1, result.diagnostic.span.column);
    EXPECT_STR_EQ("unexpected character '@'", result.diagnostic.message);

    lil_result_free(&result);
}

/* §4.1, §6.1 */
TEST(run_source_returns_syntax_diagnostic) {
    Lil_Result result;

    EXPECT_FALSE(lil_run_source("bad.lil", "x + ;", 5, &result));
    EXPECT_EQ(DIAGNOSTIC_SYNTAX, result.diagnostic.stage);
    EXPECT_EQ(4, result.diagnostic.span.offset);
    EXPECT_EQ(1, result.diagnostic.span.line);
    EXPECT_EQ(5, result.diagnostic.span.column);
    EXPECT_STR_EQ("expected expression", result.diagnostic.message);

    lil_result_free(&result);
}

/* §4.4 the left operand of '=' must be a single identifier. */
TEST(run_source_rejects_invalid_assignment_target) {
    Lil_Result result;

    EXPECT_FALSE(lil_run_source("bad.lil", "x + y = z;", 10, &result));
    EXPECT_EQ(DIAGNOSTIC_SYNTAX, result.diagnostic.stage);
    EXPECT_EQ(6, result.diagnostic.span.offset);
    EXPECT_STR_EQ("invalid assignment target", result.diagnostic.message);

    lil_result_free(&result);
}

/* §3.1 an empty file is a valid, empty program. */
TEST(run_source_accepts_empty_program) {
    Lil_Result result;

    EXPECT_TRUE(lil_run_source("empty.lil", "", 0, &result));
    EXPECT_TRUE(result.has_program);
    EXPECT_EQ(0, result.program.statement_count);

    lil_result_free(&result);
}

static void expect_result_is_cleared(const Lil_Result *result) {
    EXPECT_FALSE(result->has_program);
    EXPECT_TRUE(result->source.path == NULL);
    EXPECT_TRUE(result->source.text == NULL);
    EXPECT_TRUE(result->program.statements == NULL);
    EXPECT_EQ(0, result->program.statement_count);
    EXPECT_EQ(0, result->errors.count);
    EXPECT_FALSE(diagnostic_has_error(&result->diagnostic));
}

/* §6.3 no leaks or double frees: lil_result_free() is idempotent after success. */
TEST(result_free_is_repeatable_after_success) {
    Lil_Result result;

    EXPECT_TRUE(lil_run_source("memory.lil", "a = 1; b = 2;", 13, &result));
    lil_result_free(&result);
    lil_result_free(&result);

    expect_result_is_cleared(&result);
}

/* §6.3 no leaks or double frees: lil_result_free() is idempotent after failure. */
TEST(result_free_is_repeatable_after_failure) {
    Lil_Result result;

    EXPECT_FALSE(lil_run_source("bad.lil", "a = 1; @", 8, &result));
    lil_result_free(&result);
    lil_result_free(&result);

    expect_result_is_cleared(&result);
}

/* Lexes 'text' and returns the first token; *diagnostic gets the lexer's
 * diagnostic (empty unless the token is TOKEN_INVALID). */
static Token lex_first(const char *text, size_t length, Diagnostic *diagnostic) {
    Source source;
    Token token = {0};

    diagnostic_clear(diagnostic);

    if (!source_init(&source, "memory.lil", text, length)) {
        EXPECT_TRUE(false);
        return token;
    }

    Lexer lexer;
    lexer_init(&lexer, &source);
    token = lexer_next_token(&lexer);
    *diagnostic = *lexer_diagnostic(&lexer);
    source_free(&source);
    return token;
}

/* §2.3–§2.5, §2.8 every token kind, with keywords matched case-insensitively. */
TEST(lexer_recognizes_every_token_kind) {
    static const char input[] =
        "name 123 'x' IF Else wHiLe do BREAK continue ( ) { } ; , "
        "= + - * / & | ~ && || < <= > >= == ~= ifx .if";
    static const Token_Kind expected[] = {
        TOKEN_IDENTIFIER, TOKEN_NUMBER, TOKEN_CHARACTER,
        TOKEN_IF, TOKEN_ELSE, TOKEN_WHILE, TOKEN_DO, TOKEN_BREAK,
        TOKEN_CONTINUE,
        TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN, TOKEN_LEFT_BRACE,
        TOKEN_RIGHT_BRACE, TOKEN_SEMICOLON, TOKEN_COMMA,
        TOKEN_ASSIGN, TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH,
        TOKEN_AMPERSAND, TOKEN_PIPE, TOKEN_TILDE, TOKEN_LOGICAL_AND,
        TOKEN_LOGICAL_OR, TOKEN_LESS, TOKEN_LESS_EQUAL, TOKEN_GREATER,
        TOKEN_GREATER_EQUAL, TOKEN_EQUAL_EQUAL, TOKEN_NOT_EQUAL,
        TOKEN_IDENTIFIER, TOKEN_IDENTIFIER, TOKEN_END
    };
    Source source;

    ASSERT_TRUE(source_init(&source, "memory.lil", input, strlen(input)));

    Lexer lexer;
    lexer_init(&lexer, &source);

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const Token token = lexer_next_token(&lexer);

        if (token.kind != expected[i]) {
            fprintf(stderr, "    at token %zu\n", i);
        }

        EXPECT_EQ(expected[i], token.kind);
    }

    EXPECT_FALSE(diagnostic_has_error(lexer_diagnostic(&lexer)));
    source_free(&source);
}

/* §2.6 accepted character literals, including every escape sequence. */
TEST(lexer_accepts_character_literals) {
    static const char *const inputs[] = {
        "'a'", "'%'", "'\\0'", "'\\n'", "'\\r'", "'\\t'", "'\\\\'", "'\\''"
    };

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        Diagnostic diagnostic;
        const Token token = lex_first(inputs[i], strlen(inputs[i]), &diagnostic);

        EXPECT_EQ(TOKEN_CHARACTER, token.kind);
        EXPECT_EQ(strlen(inputs[i]), token.span.length);
        EXPECT_FALSE(diagnostic_has_error(&diagnostic));
    }
}

/* §2.6 regression: a newline or end of file before the closing quote is
 * "unterminated character literal", not the multi-character message. */
TEST(lexer_reports_unterminated_character_literal) {
    static const char *const inputs[] = {
        "'a\n'", "'a", "'", "'\n", "'\\", "'\\\n", "'\\n"
    };

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        Diagnostic diagnostic;
        const Token token = lex_first(inputs[i], strlen(inputs[i]), &diagnostic);

        EXPECT_EQ(TOKEN_INVALID, token.kind);
        EXPECT_EQ(DIAGNOSTIC_LEXICAL, diagnostic.stage);
        EXPECT_EQ(0, diagnostic.span.offset);
        EXPECT_STR_EQ("unterminated character literal", diagnostic.message);
    }
}

/* §2.6 the remaining character-literal errors. */
TEST(lexer_reports_character_literal_errors) {
    Diagnostic diagnostic;

    lex_first("''", 2, &diagnostic);
    EXPECT_STR_EQ("empty character literal", diagnostic.message);

    lex_first("'\\q'", 4, &diagnostic);
    EXPECT_STR_EQ("unsupported escape sequence", diagnostic.message);

    lex_first("'ab'", 4, &diagnostic);
    EXPECT_STR_EQ(
        "character literal contains more than one character",
        diagnostic.message
    );
    EXPECT_EQ(0, diagnostic.span.offset);
    EXPECT_EQ(3, diagnostic.span.length);

    /* The second character decides it, even before a newline. */
    lex_first("'ab\n", 4, &diagnostic);
    EXPECT_STR_EQ(
        "character literal contains more than one character",
        diagnostic.message
    );
}

/* §2.6: an unsupported escape is reported as such even when the literal
 * is also unterminated (the escape is checked first). */
TEST(lexer_reports_unsupported_escape_before_unterminated) {
    static const char *const inputs[] = { "'\\q", "'\\q\n", "'\\q'" };

    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        Diagnostic diagnostic;
        const Token token = lex_first(inputs[i], strlen(inputs[i]), &diagnostic);

        EXPECT_EQ(TOKEN_INVALID, token.kind);
        EXPECT_EQ(0, diagnostic.span.offset);
        EXPECT_EQ(1, diagnostic.span.line);
        EXPECT_EQ(1, diagnostic.span.column);
        EXPECT_STR_EQ("unsupported escape sequence", diagnostic.message);
    }
}

/* §2.2 regression: only LF ends a comment; a bare CR inside it does not. */
TEST(lexer_comment_does_not_end_at_bare_cr) {
    const char input[] = "a % x\rb = 1;\nc";
    Source source;
    Lexer lexer;
    Token token;

    ASSERT_TRUE(source_init(&source, "memory.lil", input, strlen(input)));
    lexer_init(&lexer, &source);

    token = lexer_next_token(&lexer);
    EXPECT_EQ(TOKEN_IDENTIFIER, token.kind);
    EXPECT_EQ(0, token.span.offset);

    token = lexer_next_token(&lexer);
    EXPECT_EQ(TOKEN_IDENTIFIER, token.kind);
    EXPECT_EQ(strlen(input) - 1, token.span.offset);
    EXPECT_EQ(2, token.span.line);
    EXPECT_EQ(1, token.span.column);

    token = lexer_next_token(&lexer);
    EXPECT_EQ(TOKEN_END, token.kind);
    source_free(&source);
}

/* §2.4 regression: only ASCII letters start identifiers, whatever the
 * locale. Bytes 0x80-0xFF stay unexpected characters after setlocale. */
TEST(lexer_identifiers_are_ascii_regardless_of_locale) {
    static const char *const locales[] = {
        "", "C.UTF-8", "C.utf8", "en_US.ISO-8859-1", "de_DE.ISO-8859-1",
        "fr_FR.ISO-8859-1", "en_US.UTF-8"
    };

    for (size_t l = 0; l < sizeof(locales) / sizeof(locales[0]); ++l) {
        if (setlocale(LC_ALL, locales[l]) == NULL) {
            continue;
        }

        for (int byte = 0x80; byte <= 0xff; ++byte) {
            const char text[] = { (char)byte };
            Diagnostic diagnostic;
            const Token token = lex_first(text, 1, &diagnostic);

            EXPECT_EQ(TOKEN_INVALID, token.kind);
        }

        /* An identifier stops before a non-ASCII byte; keywords still
         * match case-insensitively. */
        Diagnostic diagnostic;
        Token token = lex_first("a\xe9", 2, &diagnostic);
        EXPECT_EQ(TOKEN_IDENTIFIER, token.kind);
        EXPECT_EQ(1, token.span.length);

        token = lex_first("WHILE", 5, &diagnostic);
        EXPECT_EQ(TOKEN_WHILE, token.kind);
    }

    setlocale(LC_ALL, "C");
}

/* §2.8 regression: bytes outside printable ASCII are rendered as '\xHH',
 * so a NUL byte no longer truncates the message. */
TEST(lexer_escapes_unprintable_unexpected_character) {
    static const struct {
        char byte;
        const char *message;
    } cases[] = {
        {'\0', "unexpected character '\\x00'"},
        {'\x1b', "unexpected character '\\x1b'"},
        {'\x7f', "unexpected character '\\x7f'"},
        {'\xef', "unexpected character '\\xef'"},
        {'\xff', "unexpected character '\\xff'"},
        {'#', "unexpected character '#'"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Diagnostic diagnostic;
        const Token token = lex_first(&cases[i].byte, 1, &diagnostic);

        EXPECT_EQ(TOKEN_INVALID, token.kind);
        EXPECT_STR_EQ(cases[i].message, diagnostic.message);
    }
}

/* §2.6 a character literal's value is the unsigned byte value. */
TEST(lexer_computes_character_literal_values) {
    static const struct {
        const char *text;
        int64_t value;
    } cases[] = {
        {"'a'", 97}, {"'%'", 37}, {"'\\0'", 0}, {"'\\n'", 10},
        {"'\\r'", 13}, {"'\\t'", 9}, {"'\\\\'", 92}, {"'\\''", 39},
        {"'\xff'", 255}, {"'\x80'", 128}, {"' '", 32},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Diagnostic diagnostic;
        const Token token =
            lex_first(cases[i].text, strlen(cases[i].text), &diagnostic);

        EXPECT_EQ(TOKEN_CHARACTER, token.kind);
        EXPECT_EQ(cases[i].value, token.value);
    }
}

/* §2.7.1 decimal and §2.7.2 octal literal values (leading 0 = octal). */
TEST(lexer_computes_integer_literal_values) {
    static const struct {
        const char *text;
        int64_t value;
    } cases[] = {
        {"0", 0}, {"7", 7}, {"10", 10}, {"123", 123},
        {"00", 0}, {"010", 8}, {"0777", 511}, {"017", 15},
        {"000000000000000000000001", 1},
        {"9223372036854775807", INT64_MAX},
        {"0777777777777777777777", INT64_MAX},
        {"0000777777777777777777777", INT64_MAX},
        {"9223372036854775806", INT64_MAX - 1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        Diagnostic diagnostic;
        const Token token =
            lex_first(cases[i].text, strlen(cases[i].text), &diagnostic);

        if (token.kind != TOKEN_NUMBER || token.value != cases[i].value) {
            fprintf(stderr, "    input %s\n", cases[i].text);
        }

        EXPECT_EQ(TOKEN_NUMBER, token.kind);
        EXPECT_EQ(strlen(cases[i].text), token.span.length);
        EXPECT_EQ(cases[i].value, token.value);
        EXPECT_FALSE(diagnostic_has_error(&diagnostic));
    }
}

/* §2.7.2 '8' and '9' are not octal digits; §2.7.3 range is INT64_MAX.
 * Both errors cover the whole digit string; the octal check comes first. */
TEST(lexer_reports_integer_literal_errors) {
    static const struct {
        const char *text;
        const char *message;
    } cases[] = {
        {"08", "invalid digit '8' in octal literal"},
        {"09", "invalid digit '9' in octal literal"},
        {"0779", "invalid digit '9' in octal literal"},
        {"0789", "invalid digit '8' in octal literal"},
        {"0999999999999999999999999999", "invalid digit '9' in octal literal"},
        {"9223372036854775808", "integer literal out of range"},
        {"9223372036854775810", "integer literal out of range"},
        {"18446744073709551616", "integer literal out of range"},
        {"99999999999999999999999999999", "integer literal out of range"},
        {"01000000000000000000000", "integer literal out of range"},
        {"0177777777777777777777777", "integer literal out of range"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const size_t length = strlen(cases[i].text);
        char text[64];
        Diagnostic diagnostic;

        /* Followed by more input: the error span is the digit string only. */
        snprintf(text, sizeof(text), "%s;", cases[i].text);

        const Token token = lex_first(text, length + 1, &diagnostic);

        if (strcmp(cases[i].message, diagnostic.message) != 0) {
            fprintf(stderr, "    input %s\n", cases[i].text);
        }

        EXPECT_EQ(TOKEN_INVALID, token.kind);
        EXPECT_EQ(DIAGNOSTIC_LEXICAL, diagnostic.stage);
        EXPECT_EQ(0, diagnostic.span.offset);
        EXPECT_EQ(length, diagnostic.span.length);
        EXPECT_EQ(length, token.span.length);
        EXPECT_STR_EQ(cases[i].message, diagnostic.message);
    }
}

/* Lexes all of 'text' and compares the first 'count' tokens with
 * kinds[] and spellings[]; a final empty TOKEN_END is implied. */
static void expect_token_sequence(
    const char *text,
    size_t length,
    const Token_Kind *kinds,
    const char *const *spellings,
    size_t count
) {
    Source source;

    if (!source_init(&source, "memory.lil", text, length)) {
        EXPECT_TRUE(false);
        return;
    }

    Lexer lexer;
    lexer_init(&lexer, &source);

    for (size_t i = 0; i <= count; ++i) {
        const Token token = lexer_next_token(&lexer);
        const Token_Kind kind = i < count ? kinds[i] : TOKEN_END;
        const char *spelling = i < count ? spellings[i] : "";

        if (token.kind != kind) {
            fprintf(stderr, "    input \"%s\", token %zu\n", text, i);
        }

        EXPECT_EQ(kind, token.kind);
        EXPECT_MEM_EQ(
            spelling,
            source_text_at(&source, token.span),
            token.span.length
        );
    }

    EXPECT_FALSE(diagnostic_has_error(lexer_diagnostic(&lexer)));
    source_free(&source);
}

/* §2.3 longest match and number/identifier splitting. */
TEST(lexer_applies_longest_match) {
    {
        static const Token_Kind kinds[] = {
            TOKEN_NUMBER, TOKEN_IDENTIFIER, TOKEN_NUMBER, TOKEN_IDENTIFIER
        };
        static const char *const spellings[] = {"123", "abc", "123", ".4"};
        expect_token_sequence("123abc 123.4", 12, kinds, spellings, 4);
    }
    {
        static const Token_Kind kinds[] = {
            TOKEN_NUMBER, TOKEN_IDENTIFIER, TOKEN_NUMBER, TOKEN_IDENTIFIER
        };
        static const char *const spellings[] = {"0", "x10", "010", "_8"};
        expect_token_sequence("0x10 010_8", 10, kinds, spellings, 4);
    }
    {
        static const Token_Kind kinds[] = {
            TOKEN_IDENTIFIER, TOKEN_NOT_EQUAL, TOKEN_IDENTIFIER,
            TOKEN_IDENTIFIER, TOKEN_ASSIGN, TOKEN_TILDE, TOKEN_IDENTIFIER,
            TOKEN_LESS_EQUAL, TOKEN_ASSIGN, TOKEN_LOGICAL_AND,
            TOKEN_AMPERSAND, TOKEN_LOGICAL_OR, TOKEN_PIPE, TOKEN_MINUS,
            TOKEN_MINUS, TOKEN_EQUAL_EQUAL, TOKEN_ASSIGN
        };
        static const char *const spellings[] = {
            "a", "~=", "b", "x", "=", "~", "y", "<=", "=", "&&", "&",
            "||", "|", "-", "-", "==", "="
        };
        expect_token_sequence(
            "a~=b x=~y <==&&&|||--===", 24, kinds, spellings, 17
        );
    }
    {
        static const Token_Kind kinds[] = {
            TOKEN_IDENTIFIER, TOKEN_IDENTIFIER, TOKEN_IDENTIFIER,
            TOKEN_IDENTIFIER, TOKEN_IF, TOKEN_IDENTIFIER, TOKEN_IDENTIFIER
        };
        static const char *const spellings[] = {
            ".loop", "r0.tmp", ".", "ifx", "iF", "if.loop", "putc"
        };
        expect_token_sequence(
            ".loop r0.tmp . ifx iF if.loop putc", 34, kinds, spellings, 7
        );
    }
}

/* Minimal deterministic PRNG (xorshift64*) so the property test is
 * reproducible. */
static uint64_t test_random_next(uint64_t *state) {
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * UINT64_C(2685821657736338717);
}

/* Builds pseudo-random lexer input from fragments that are mostly valid,
 * with the occasional arbitrary byte. */
static size_t make_random_input(uint64_t *state, char *buffer, size_t capacity) {
    static const char *const fragments[] = {
        " ", "  ", "\t", "\r\n", "\n", "% comment\n", "%", "x", "r0", ".",
        "_", ".loop", "IF", "if", "Else", "while", "do", "break",
        "continue", "putc", "0", "7", "010", "123", "08",
        "9223372036854775807", "9223372036854775808", "'a'", "'\\n'",
        "'\\''", "''", "'ab'", "'", "\\", "(", ")", "{", "}", ";", ",",
        "=", "==", "+", "-", "*", "/", "&", "&&", "|", "||", "~", "~=",
        "<", "<=", ">", ">=", "@", "\"", "\xc3\xa9", "\r", "% a\rb\n",
        "%\r", "'\\q", "\x0c"
    };
    const size_t fragment_count = sizeof(fragments) / sizeof(fragments[0]);
    const size_t target = (size_t)(test_random_next(state) % (capacity / 2));
    size_t length = 0;

    while (length < target) {
        const uint64_t choice = test_random_next(state);

        if (choice % 50 == 0) {
            buffer[length++] = (char)(test_random_next(state) & 0xff);
            continue;
        }

        const char *fragment = fragments[(choice >> 8) % fragment_count];
        const size_t fragment_length = strlen(fragment);

        if (length + fragment_length > capacity) {
            break;
        }

        memcpy(buffer + length, fragment, fragment_length);
        length += fragment_length;
    }

    return length;
}

/* True when text[start, end) consists only of whitespace and comments
 * (§2.1, §2.2), where every comment ends at a LF inside the gap or at the
 * end of the 'length'-byte input. */
static bool is_ignored_text(
    const char *text,
    size_t length,
    size_t start,
    size_t end
) {
    size_t i = start;

    while (i < end) {
        const char c = text[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
        } else if (c == '%') {
            while (i < length && text[i] != '\n') {
                ++i;
            }

            /* A comment runs to LF or the end of the file (§2.2); one that
             * stops short of both, right before a token, is wrong. */
            if (i > end) {
                return false;
            }

            if (i == end && end < length) {
                return false;
            }
        } else {
            return false;
        }
    }

    return true;
}

/* §2.1–§2.3 property: for pseudo-random inputs, token spans are ordered,
 * non-overlapping and in bounds; line/column agree with the offset; the
 * text between tokens is only whitespace and comments (so the tokens plus
 * the skipped text reconstruct the source); every token re-lexes to
 * itself on its own; and every input ends with TOKEN_END at the end of the
 * file or with TOKEN_INVALID whose diagnostic has the token's position. */
TEST(lexer_property_spans_partition_random_input) {
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    size_t valid_inputs = 0;
    size_t invalid_inputs = 0;

    for (size_t iteration = 0; iteration < 3000; ++iteration) {
        char text[512];
        const size_t length = make_random_input(&state, text, sizeof(text));
        Source source;

        ASSERT_TRUE(source_init(&source, "random.lil", text, length));

        Lexer lexer;
        lexer_init(&lexer, &source);

        size_t previous_end = 0;
        size_t line = 1;
        size_t column = 1;
        bool ok = true;
        bool terminated = false;

        for (size_t steps = 0; steps <= length + 1 && ok; ++steps) {
            const Token token = lexer_next_token(&lexer);

            ok = token.span.offset >= previous_end &&
                 token.span.offset <= length &&
                 token.span.length <= length - token.span.offset;
            EXPECT_TRUE(ok);

            if (!ok) {
                break;
            }

            /* Gap since the previous token: only ignored text. This also
             * holds before an invalid token. */
            ok = is_ignored_text(
                text,
                length,
                previous_end,
                token.span.offset
            );
            EXPECT_TRUE(ok);

            for (size_t i = previous_end; i < token.span.offset; ++i) {
                if (text[i] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }

            EXPECT_EQ(line, token.span.line);
            EXPECT_EQ(column, token.span.column);

            if (token.kind == TOKEN_INVALID) {
                const Diagnostic *diagnostic = lexer_diagnostic(&lexer);

                EXPECT_TRUE(diagnostic_has_error(diagnostic));
                EXPECT_EQ(DIAGNOSTIC_LEXICAL, diagnostic->stage);
                EXPECT_EQ(token.span.offset, diagnostic->span.offset);
                EXPECT_EQ(line, diagnostic->span.line);
                EXPECT_EQ(column, diagnostic->span.column);
                EXPECT_TRUE(diagnostic->span.length > 0);
                EXPECT_TRUE(
                    diagnostic->span.length <= length - token.span.offset
                );
                terminated = true;
                ++invalid_inputs;
                break;
            }

            if (token.kind == TOKEN_END) {
                EXPECT_EQ(length, token.span.offset);
                EXPECT_EQ(0, token.span.length);
                EXPECT_FALSE(diagnostic_has_error(lexer_diagnostic(&lexer)));
                terminated = true;
                ++valid_inputs;
                break;
            }

            EXPECT_TRUE(token.span.length > 0);

            /* Longest match: the token's own text lexes to the same token. */
            Diagnostic diagnostic;
            const Token alone = lex_first(
                text + token.span.offset,
                token.span.length,
                &diagnostic
            );
            EXPECT_EQ(token.kind, alone.kind);
            EXPECT_EQ(token.span.length, alone.span.length);
            EXPECT_EQ(token.value, alone.value);

            for (size_t i = 0; i < token.span.length; ++i) {
                if (text[token.span.offset + i] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }

            previous_end = token.span.offset + token.span.length;
        }

        /* Every input ends in End or Invalid within length + 2 tokens. */
        if (ok) {
            ok = terminated;
            EXPECT_TRUE(terminated);
        }

        source_free(&source);

        if (!ok) {
            fprintf(stderr, "    iteration %zu\n", iteration);
            break;
        }
    }

    /* The generator must exercise both outcomes. */
    EXPECT_TRUE(valid_inputs > 100);
    EXPECT_TRUE(invalid_inputs > 100);
}

/* Token kind names are used by --dump-tokens (§6.4). */
TEST(token_kind_name_covers_every_kind) {
    static const struct {
        Token_Kind kind;
        const char *name;
    } names[] = {
        {TOKEN_END, "End"}, {TOKEN_INVALID, "Invalid"},
        {TOKEN_IDENTIFIER, "Identifier"}, {TOKEN_NUMBER, "Number"},
        {TOKEN_CHARACTER, "Character"}, {TOKEN_IF, "If"},
        {TOKEN_ELSE, "Else"}, {TOKEN_WHILE, "While"}, {TOKEN_DO, "Do"},
        {TOKEN_BREAK, "Break"}, {TOKEN_CONTINUE, "Continue"},
        {TOKEN_LEFT_PAREN, "LeftParen"}, {TOKEN_RIGHT_PAREN, "RightParen"},
        {TOKEN_LEFT_BRACE, "LeftBrace"}, {TOKEN_RIGHT_BRACE, "RightBrace"},
        {TOKEN_SEMICOLON, "Semicolon"}, {TOKEN_COMMA, "Comma"},
        {TOKEN_ASSIGN, "Assign"}, {TOKEN_PLUS, "Plus"},
        {TOKEN_MINUS, "Minus"}, {TOKEN_STAR, "Star"},
        {TOKEN_SLASH, "Slash"}, {TOKEN_AMPERSAND, "Ampersand"},
        {TOKEN_PIPE, "Pipe"}, {TOKEN_TILDE, "Tilde"},
        {TOKEN_LOGICAL_AND, "LogicalAnd"}, {TOKEN_LOGICAL_OR, "LogicalOr"},
        {TOKEN_LESS, "Less"}, {TOKEN_LESS_EQUAL, "LessEqual"},
        {TOKEN_GREATER, "Greater"}, {TOKEN_GREATER_EQUAL, "GreaterEqual"},
        {TOKEN_EQUAL_EQUAL, "EqualEqual"}, {TOKEN_NOT_EQUAL, "NotEqual"},
    };
    const size_t count = sizeof(names) / sizeof(names[0]);

    /* The table lists every enumerator exactly once, in order. */
    EXPECT_EQ(TOKEN_NOT_EQUAL + 1, count);

    for (size_t i = 0; i < count; ++i) {
        EXPECT_EQ(i, names[i].kind);
        EXPECT_STR_EQ(names[i].name, token_kind_name(names[i].kind));
    }

    EXPECT_STR_EQ("Unknown", token_kind_name((Token_Kind)(TOKEN_NOT_EQUAL + 1)));
}

/* §3.2 an expression statement must end with ';'. */
TEST(run_source_requires_semicolon) {
    Lil_Result result;

    EXPECT_FALSE(lil_run_source("bad.lil", "x = 1 y", 7, &result));
    EXPECT_EQ(DIAGNOSTIC_SYNTAX, result.diagnostic.stage);
    EXPECT_EQ(6, result.diagnostic.span.offset);
    EXPECT_STR_EQ("expected ';' after expression", result.diagnostic.message);

    lil_result_free(&result);
}

/* Builds prefix + repeat * count + suffix in a malloc'd buffer. */
static char *repeat_text(
    const char *prefix,
    const char *repeat,
    size_t count,
    const char *suffix,
    size_t *out_length
) {
    const size_t prefix_length = strlen(prefix);
    const size_t repeat_length = strlen(repeat);
    const size_t suffix_length = strlen(suffix);
    const size_t length =
        prefix_length + repeat_length * count + suffix_length;
    char *text = malloc(length + 1);

    if (text == NULL) {
        return NULL;
    }

    char *cursor = text;
    memcpy(cursor, prefix, prefix_length);
    cursor += prefix_length;

    for (size_t i = 0; i < count; ++i) {
        memcpy(cursor, repeat, repeat_length);
        cursor += repeat_length;
    }

    memcpy(cursor, suffix, suffix_length + 1);
    *out_length = length;
    return text;
}

/* Runs prefix + repeat * count + suffix; returns whether it parsed. When
 * it did not, expects "nesting too deep" at byte 'error_offset'. */
static bool run_repeated(
    const char *prefix,
    const char *repeat,
    size_t count,
    const char *suffix,
    size_t error_offset
) {
    size_t length = 0;
    char *text = repeat_text(prefix, repeat, count, suffix, &length);

    EXPECT_TRUE(text != NULL);

    if (text == NULL) {
        return false;
    }

    Lil_Result result;
    const bool succeeded = lil_run_source("deep.lil", text, length, &result);

    if (!succeeded) {
        EXPECT_EQ(DIAGNOSTIC_SYNTAX, result.diagnostic.stage);
        EXPECT_EQ(error_offset, result.diagnostic.span.offset);
        EXPECT_STR_EQ("nesting too deep", result.diagnostic.message);
    }

    lil_result_free(&result);
    free(text);
    return succeeded;
}

/* §6.3 assignment chains nest at most PARSER_MAX_NESTING deep. */
TEST(parser_limits_assignment_nesting) {
    EXPECT_TRUE(run_repeated("", "a = ", PARSER_MAX_NESTING, "1;", 0));

    /* The first '=' beyond the limit is reported. */
    EXPECT_FALSE(run_repeated(
        "", "a = ", PARSER_MAX_NESTING + 1, "1;",
        4 * PARSER_MAX_NESTING + 2
    ));
}

/* §6.3 regression: 60000 chained assignments used to overflow the stack. */
TEST(parser_rejects_huge_assignment_chain_without_crashing) {
    EXPECT_FALSE(run_repeated(
        "x = ", "a = ", 60000, "1;",
        4 * PARSER_MAX_NESTING + 2
    ));
}

/* §6.3 expression trees are at most PARSER_MAX_EXPR_HEIGHT tall,
 * including left-associative operator chains. */
TEST(parser_limits_expression_height) {
    /* n '+' operators make a tree of height n + 1. */
    EXPECT_TRUE(run_repeated("1", " + 1", PARSER_MAX_EXPR_HEIGHT - 1, ";", 0));
    EXPECT_TRUE(run_repeated("1", " * 1", PARSER_MAX_EXPR_HEIGHT - 1, ";", 0));

    /* The operator that would make the tree too tall is reported. */
    EXPECT_FALSE(run_repeated(
        "1", " + 1", PARSER_MAX_EXPR_HEIGHT, ";",
        4 * (PARSER_MAX_EXPR_HEIGHT - 1) + 2
    ));

    /* The assignment node counts too. */
    EXPECT_FALSE(run_repeated(
        "x = 1", " + 1", PARSER_MAX_EXPR_HEIGHT - 1, ";", 2
    ));
}

/* §6.3 regression: a 1M-term operator chain used to overflow the stack
 * when the AST was freed or printed. */
TEST(parser_rejects_huge_operator_chain_without_crashing) {
    EXPECT_FALSE(run_repeated(
        "1", "+1", 1000000, ";",
        2 * (PARSER_MAX_EXPR_HEIGHT - 1) + 1
    ));
    EXPECT_FALSE(run_repeated(
        "1", "*1", 1000000, ";",
        2 * (PARSER_MAX_EXPR_HEIGHT - 1) + 1
    ));
}

/* §3.1 regression: the statement array grows geometrically, so many
 * statements parse in linear time (this took ~30 s under ASan before). */
TEST(parser_handles_many_statements) {
    const size_t count = 200000;
    size_t length = 0;
    char *text = repeat_text("", "x = 1;\n", count, "", &length);

    ASSERT_TRUE(text != NULL);

    Lil_Result result;
    EXPECT_TRUE(lil_run_source("many.lil", text, length, &result));
    EXPECT_EQ(count, result.program.statement_count);

    lil_result_free(&result);
    free(text);
}

/* ---- Stage 2: parser ---------------------------------------------------- */

/* A growable text buffer for canonical renderings and generated programs. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Text;

static void text_append(Text *text, const char *bytes, size_t length) {
    if (text->length + length + 1 > text->capacity) {
        size_t capacity = text->capacity == 0 ? 256 : text->capacity;

        while (text->length + length + 1 > capacity) {
            capacity *= 2;
        }

        char *grown = realloc(text->data, capacity);

        if (grown == NULL) {
            abort();
        }

        text->data = grown;
        text->capacity = capacity;
    }

    memcpy(text->data + text->length, bytes, length);
    text->length += length;
    text->data[text->length] = '\0';
}

static void text_puts(Text *text, const char *string) {
    text_append(text, string, strlen(string));
}

static void text_token(Text *text, const Source *source, Token token) {
    text_append(text, source_text_at(source, token.span), token.span.length);
}

/*
 * Canonical, span-free rendering used to compare ASTs: every unary,
 * binary and assignment node is parenthesized, and every statement branch
 * is bracketed, so two different trees never render alike.
 */
static void render_expr(Text *out, const Source *source, const Expr *expr) {
    switch (expr->kind) {
        case EXPR_VARIABLE:
            text_token(out, source, expr->as.variable);
            break;

        case EXPR_LITERAL:
            text_token(out, source, expr->as.literal);
            break;

        case EXPR_UNARY:
            text_puts(out, "(");
            text_token(out, source, expr->as.unary.operator_token);
            render_expr(out, source, expr->as.unary.operand);
            text_puts(out, ")");
            break;

        case EXPR_BINARY:
            text_puts(out, "(");
            render_expr(out, source, expr->as.binary.left);
            text_puts(out, " ");
            text_token(out, source, expr->as.binary.operator_token);
            text_puts(out, " ");
            render_expr(out, source, expr->as.binary.right);
            text_puts(out, ")");
            break;

        case EXPR_ASSIGNMENT:
            text_puts(out, "(");
            text_token(out, source, expr->as.assignment.name);
            text_puts(out, " = ");
            render_expr(out, source, expr->as.assignment.value);
            text_puts(out, ")");
            break;

        case EXPR_CALL:
            text_token(out, source, expr->as.call.name);
            text_puts(out, "(");

            for (size_t i = 0; i < expr->as.call.argument_count; ++i) {
                if (i > 0) {
                    text_puts(out, ", ");
                }

                render_expr(out, source, expr->as.call.arguments[i]);
            }

            text_puts(out, ")");
            break;
    }
}

static void render_stmts(
    Text *out,
    const Source *source,
    Stmt *const *statements,
    size_t count
);

static void render_stmt(Text *out, const Source *source, const Stmt *stmt) {
    switch (stmt->kind) {
        case STMT_EXPRESSION:
            render_expr(out, source, stmt->as.expression);
            text_puts(out, ";");
            break;

        case STMT_EMPTY:
            text_puts(out, ";");
            break;

        case STMT_BLOCK:
            text_puts(out, "{");
            render_stmts(
                out,
                source,
                stmt->as.block.statements,
                stmt->as.block.statement_count
            );
            text_puts(out, "}");
            break;

        case STMT_IF:
            text_puts(out, "if (");
            render_expr(out, source, stmt->as.if_statement.condition);
            text_puts(out, ") [");
            render_stmt(out, source, stmt->as.if_statement.then_branch);
            text_puts(out, "]");

            if (stmt->as.if_statement.else_branch != NULL) {
                text_puts(out, " else [");
                render_stmt(out, source, stmt->as.if_statement.else_branch);
                text_puts(out, "]");
            }
            break;

        case STMT_WHILE:
            text_puts(out, "while (");
            render_expr(out, source, stmt->as.while_statement.condition);
            text_puts(out, ") [");
            render_stmt(out, source, stmt->as.while_statement.body);
            text_puts(out, "]");
            break;

        case STMT_DO:
            text_puts(out, "do [");
            render_stmt(out, source, stmt->as.do_statement.body);
            text_puts(out, "] while (");
            render_expr(out, source, stmt->as.do_statement.condition);
            text_puts(out, ");");
            break;

        case STMT_BREAK:
            text_puts(out, "break;");
            break;

        case STMT_CONTINUE:
            text_puts(out, "continue;");
            break;
    }
}

static void render_stmts(
    Text *out,
    const Source *source,
    Stmt *const *statements,
    size_t count
) {
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            text_puts(out, " ");
        }

        render_stmt(out, source, statements[i]);
    }
}

/* Canonical rendering of a whole program (malloc'd, never NULL). */
static char *render_program(const Source *source, const Program *program) {
    Text out = {0};

    text_puts(&out, "");
    render_stmts(&out, source, program->statements, program->statement_count);
    return out.data;
}

/* Parses 'text'; returns its canonical rendering, or NULL when it did not
 * parse (the first error is stored in *first_error when not NULL). */
static char *parse_render(const char *text, Diagnostic *first_error) {
    Lil_Result result;
    char *rendering = NULL;

    if (lil_run_source("test.lil", text, strlen(text), &result)) {
        rendering = render_program(&result.source, &result.program);
    } else if (first_error != NULL) {
        *first_error = result.diagnostic;
    }

    lil_result_free(&result);
    return rendering;
}

/* Checks that 'text' parses and renders canonically as 'expected'. */
static void expect_parses_as(const char *text, const char *expected) {
    Diagnostic error;
    char *rendering = parse_render(text, &error);

    if (rendering == NULL) {
        test_check_str(expected, error.message, "parse of", text,
                       __FILE__, __LINE__);
        return;
    }

    test_check_str(expected, rendering, text, "rendering", __FILE__, __LINE__);
    free(rendering);
}

/* Checks that 'text' fails with exactly the given errors (source order). */
static void expect_errors(
    const char *text,
    size_t count,
    const size_t *offsets,
    const char *const *messages
) {
    Lil_Result result;

    EXPECT_FALSE(lil_run_source("test.lil", text, strlen(text), &result));
    EXPECT_FALSE(result.has_program);
    EXPECT_TRUE(result.program.statements == NULL);
    test_check_uint(count, result.errors.count, text, "error count",
                    __FILE__, __LINE__);

    for (size_t i = 0; i < count && i < result.errors.count; ++i) {
        test_check_uint(offsets[i], result.errors.items[i].span.offset,
                        text, "error offset", __FILE__, __LINE__);
        test_check_str(messages[i], result.errors.items[i].message,
                       text, "error message", __FILE__, __LINE__);
    }

    if (result.errors.count > 0) {
        EXPECT_EQ(result.errors.items[0].span.offset,
                  result.diagnostic.span.offset);
        EXPECT_STR_EQ(result.errors.items[0].message,
                      result.diagnostic.message);
    }

    lil_result_free(&result);
}

static void expect_error(const char *text, size_t offset, const char *message) {
    expect_errors(text, 1, &offset, &message);
}

/* Independent oracle for §4.2 (deliberately not src/operator.c). */
static const struct {
    const char *spelling;
    int level;
    bool right;
} spec_binary_operators[] = {
    {"=", 1, true},
    {"||", 2, false},
    {"&&", 3, false},
    {"|", 4, false},
    {"&", 5, false},
    {"==", 6, false},
    {"~=", 6, false},
    {"<", 7, false},
    {"<=", 7, false},
    {">", 7, false},
    {">=", 7, false},
    {"+", 8, false},
    {"-", 8, false},
    {"*", 9, false},
    {"/", 9, false},
};

#define SPEC_BINARY_COUNT \
    (sizeof(spec_binary_operators) / sizeof(spec_binary_operators[0]))

/* §4.2 the examples named in the spec and the plan. */
TEST(parser_precedence_examples) {
    expect_parses_as("a || b && c;", "(a || (b && c));");
    expect_parses_as("a && b || c;", "((a && b) || c);");
    expect_parses_as("a = b = c;", "(a = (b = c));");
    expect_parses_as("-a * b;", "((-a) * b);");
    expect_parses_as("a < b == c;", "((a < b) == c);");
    expect_parses_as("a & b == c;", "(a & (b == c));");
    expect_parses_as("a - b - c;", "((a - b) - c);");
    expect_parses_as("a / b / c;", "((a / b) / c);");
    expect_parses_as("a < b < c;", "((a < b) < c);");
    expect_parses_as("a | b & c;", "(a | (b & c));");
    expect_parses_as("a + b * c - d / e;", "((a + (b * c)) - (d / e));");
    expect_parses_as("x = a || b;", "(x = (a || b));");
    expect_parses_as("~-a;", "(~(-a));");
    expect_parses_as("- -a;", "(-(-a));");
    expect_parses_as("--a;", "(-(-a));");
    expect_parses_as("a ~= ~b;", "(a ~= (~b));");
    expect_parses_as("x=~y;", "(x = (~y));");
}

/* §4.2 every ordered pair of binary operators, 'a OP1 b OP2 c'. */
TEST(parser_binary_operator_pairs_follow_precedence_table) {
    for (size_t i = 0; i < SPEC_BINARY_COUNT; ++i) {
        for (size_t j = 0; j < SPEC_BINARY_COUNT; ++j) {
            const char *op1 = spec_binary_operators[i].spelling;
            const char *op2 = spec_binary_operators[j].spelling;
            const int level1 = spec_binary_operators[i].level;
            const int level2 = spec_binary_operators[j].level;
            const bool left_first =
                level1 > level2 ||
                (level1 == level2 && !spec_binary_operators[i].right);
            char text[64];
            char expected[64];

            snprintf(text, sizeof(text), "a %s b %s c;", op1, op2);

            if (left_first && strcmp(op2, "=") == 0) {
                /* '(a OP1 b) = c': §4.4 the target is not a name. */
                expect_error(text, 5 + strlen(op1),
                             "invalid assignment target");
                continue;
            }

            if (left_first) {
                snprintf(expected, sizeof(expected), "((a %s b) %s c);",
                         op1, op2);
            } else {
                snprintf(expected, sizeof(expected), "(a %s (b %s c));",
                         op1, op2);
            }

            expect_parses_as(text, expected);
        }
    }
}

/* §4.2 prefix '-' and '~' bind tighter than every binary operator. */
TEST(parser_unary_operators_bind_tighter_than_binary) {
    static const char *const unary[] = {"-", "~"};

    for (size_t u = 0; u < 2; ++u) {
        for (size_t i = 0; i < SPEC_BINARY_COUNT; ++i) {
            const char *op = spec_binary_operators[i].spelling;
            char text[64];
            char expected[64];

            if (strcmp(op, "=") == 0) {
                snprintf(text, sizeof(text), "a = %sb;", unary[u]);
                snprintf(expected, sizeof(expected), "(a = (%sb));",
                         unary[u]);
                expect_parses_as(text, expected);

                snprintf(text, sizeof(text), "%sa = b;", unary[u]);
                expect_error(text, 3, "invalid assignment target");
                continue;
            }

            snprintf(text, sizeof(text), "%sa %s b;", unary[u], op);
            snprintf(expected, sizeof(expected), "((%sa) %s b);", unary[u], op);
            expect_parses_as(text, expected);

            snprintf(text, sizeof(text), "a %s %sb;", op, unary[u]);
            snprintf(expected, sizeof(expected), "(a %s (%sb));", op, unary[u]);
            expect_parses_as(text, expected);
        }
    }
}

/* §4.1 parentheses group and leave no node behind. */
TEST(parser_parentheses_group) {
    expect_parses_as("(a + b) * c;", "((a + b) * c);");
    expect_parses_as("a * (b + c);", "(a * (b + c));");
    expect_parses_as("((a));", "a;");
    expect_parses_as("a - (b - c);", "(a - (b - c));");
    expect_parses_as("x = (y = 1) + 2;", "(x = ((y = 1) + 2));");
    expect_parses_as("-(a + b);", "(-(a + b));");
}

/* §4.1 calls: any name, any number of arguments (checked in stage 3). */
TEST(parser_parses_calls) {
    expect_parses_as("putc(x);", "putc(x);");
    expect_parses_as("PUTC('a');", "PUTC('a');");
    expect_parses_as("f();", "f();");
    expect_parses_as("f (1);", "f(1);");
    expect_parses_as("f(a, b = 1, -c);", "f(a, (b = 1), (-c));");
    expect_parses_as("putc(f(g(1)) + 2);", "putc((f(g(1)) + 2));");
    expect_parses_as("x = putc(10) * 2;", "(x = (putc(10) * 2));");
    expect_parses_as("f(x) - 1;", "(f(x) - 1);");
}

/* §3 every statement form, keywords in any case (§2.5). */
TEST(parser_parses_statements) {
    expect_parses_as(";", ";");
    expect_parses_as(";;", "; ;");
    expect_parses_as("{}", "{}");
    expect_parses_as("{ a; {} ; }", "{a; {} ;}");
    expect_parses_as("if (a) b;", "if (a) [b;]");
    expect_parses_as("if (a) b; else c;", "if (a) [b;] else [c;]");
    expect_parses_as("IF (a) {} ElSe {}", "if (a) [{}] else [{}]");
    expect_parses_as(
        "if (a) b; else if (c) d; else e;",
        "if (a) [b;] else [if (c) [d;] else [e;]]"
    );
    expect_parses_as("while (a) a = a - 1;", "while (a) [(a = (a - 1));]");
    expect_parses_as("WHILE (1) ;", "while (1) [;]");
    expect_parses_as("do x; while (y);", "do [x;] while (y);");
    expect_parses_as("Do {} WHILE (0);", "do [{}] while (0);");
    expect_parses_as("break; continue;", "break; continue;");
    expect_parses_as("BREAK; Continue;", "break; continue;");
    expect_parses_as(
        "while (a) do { break; continue; } while (b);",
        "while (a) [do [{break; continue;}] while (b);]"
    );
}

/* §3.4 dangling else binds to the nearest unmatched 'if'. */
TEST(parser_binds_dangling_else_to_nearest_if) {
    expect_parses_as(
        "if (a) if (b) x = 1; else x = 2;",
        "if (a) [if (b) [(x = 1);] else [(x = 2);]]"
    );
    expect_parses_as(
        "if (a) { if (b) x; } else y;",
        "if (a) [{if (b) [x;]}] else [y;]"
    );
    expect_parses_as(
        "if (a) if (b) if (c) x; else y; else z;",
        "if (a) [if (b) [if (c) [x;] else [y;]] else [z;]]"
    );
    expect_parses_as(
        "while (a) if (b) x; else y;",
        "while (a) [if (b) [x;] else [y;]]"
    );
}

static void expect_span(Source_Span span, size_t offset, size_t length) {
    EXPECT_EQ(offset, span.offset);
    EXPECT_EQ(length, span.length);
}

/* §6.4.3 every node has a span; parentheses are inside it. */
TEST(parser_records_node_spans) {
    const char *text =
        "(a + b) * c;\n"            /* 0 */
        "if (x) y; else { }\n"      /* 13 */
        "do ; while (z);\n"         /* 32 */
        "f(1, 2);";                 /* 48 */
    Lil_Result result;

    EXPECT_TRUE(lil_run_source("span.lil", text, strlen(text), &result));
    expect_span(result.program.span, 0, strlen(text));
    EXPECT_EQ(1, result.program.span.line);
    EXPECT_EQ(1, result.program.span.column);

    if (result.program.statement_count == 4) {
        Stmt *const *stmts = result.program.statements;
        const Expr *product = stmts[0]->as.expression;

        expect_span(stmts[0]->span, 0, 12);
        expect_span(product->span, 0, 11);
        expect_span(product->as.binary.left->span, 0, 7);
        expect_span(product->as.binary.left->as.binary.left->span, 1, 1);

        expect_span(stmts[1]->span, 13, 18);
        EXPECT_EQ(2, stmts[1]->span.line);
        expect_span(stmts[1]->as.if_statement.then_branch->span, 20, 2);
        expect_span(stmts[1]->as.if_statement.else_branch->span, 28, 3);

        expect_span(stmts[2]->span, 32, 15);
        expect_span(stmts[2]->as.do_statement.body->span, 35, 1);
        expect_span(stmts[2]->as.do_statement.condition->span, 44, 1);

        expect_span(stmts[3]->as.expression->span, 48, 7);
        EXPECT_EQ(2, stmts[3]->as.expression->as.call.argument_count);
    } else {
        EXPECT_EQ(4, result.program.statement_count);
    }

    lil_result_free(&result);
}

/* §3, §4 every syntax error message, at the offending token. */
TEST(parser_reports_each_syntax_error) {
    expect_error("a + ;", 4, "expected expression");
    expect_error("x = );", 4, "expected expression");
    expect_error("x = 1 y", 6, "expected ';' after expression");
    expect_error("(a + b;", 6, "expected ')' after expression");
    expect_error("putc(1 2);", 7, "expected ')' after arguments");
    expect_error("f(1,);", 4, "expected expression");
    expect_error("if a) x;", 3, "expected '(' after 'if'");
    expect_error("if (a x;", 6, "expected ')' after condition");
    expect_error("if (a) else x;", 7, "expected statement");
    expect_error("while a) x;", 6, "expected '(' after 'while'");
    expect_error("while (a x;", 9, "expected ')' after condition");
    expect_error("do x; until (a);", 6, "expected 'while' after 'do' body");
    expect_error("do x; while a;", 12, "expected '(' after 'while'");
    expect_error("do x; while (a;", 14, "expected ')' after condition");
    expect_error("do x; while (a)", 15, "expected ';' after 'do' statement");
    expect_error("break", 5, "expected ';' after 'break'");
    expect_error("continue x;", 9, "expected ';' after 'continue'");
    expect_error("{ x;", 4, "expected '}' before end of file");
    expect_error("}", 0, "unexpected '}'");
    expect_error("else x;", 0, "'else' without a matching 'if'");
    expect_error("x + y = z;", 6, "invalid assignment target");
    expect_error("(x) = 1;", 4, "invalid assignment target");
    expect_error("-x = 1;", 3, "invalid assignment target");
    expect_error("f(x) = 1;", 5, "invalid assignment target");
    expect_error("1 = x;", 2, "invalid assignment target");
    expect_error("if (x) }", 7, "expected statement");
    expect_error("if (x)", 6, "expected statement");
    expect_error("if (x) a; else", 14, "expected statement");
    expect_error("while (x) }", 10, "expected statement");
    expect_error("do else", 3, "expected statement");
}

/* §6.3.4 three independent errors give exactly three diagnostics. */
TEST(parser_recovers_and_reports_independent_errors) {
    static const size_t offsets[] = {7, 22, 35};
    static const char *const messages[] = {
        "expected expression",
        "expected expression",
        "expected ')' after condition",
    };

    expect_errors(
        "x = 1 +;\n"            /* error at ';' (7) */
        "if (y) { z = ; }\n"    /* error at ';' (22) */
        "while (a b) c = 1;\n"  /* error at 'b' (35) */
        "w = 2;\n",
        3, offsets, messages
    );
}

/* §6.3.4 recovery skips whole blocks and never cascades. */
TEST(parser_recovery_does_not_cascade) {
    expect_error("if (x +) { a = 1; b = 2; } c = 3;", 7, "expected expression");
    expect_error("{ if (x) }", 9, "expected statement");
    expect_error("{ a = 1 } b = 2;", 8, "expected ';' after expression");
    expect_error("{{{ x = 1;", 10, "expected '}' before end of file");
    expect_error("do x = ; while (y);", 7, "expected expression");
    expect_error("x = (1 + ; y = 2;", 9, "expected expression");
    expect_error("if (a) x; else y = ; z = 1;", 19, "expected expression");
    expect_error("while (a) { if (b) x; else } y;", 27, "expected statement");

    static const size_t offsets[] = {9, 34};
    static const char *const messages[] = {
        "expected ')' after condition",
        "expected expression",
    };
    expect_errors(
        "while (a b) { c = 1; d = 2; } e = ;",
        2, offsets, messages
    );

    /* A statement after an error inside a block is still parsed. */
    static const size_t block_offsets[] = {6, 19};
    static const char *const block_messages[] = {
        "expected expression",
        "expected expression",
    };
    expect_errors("{ a = ; b = 1; c = ; }", 2, block_offsets, block_messages);
}

/* §6.3.4 step 3: a tail ('else' or 'while (...) ;') that step 2 already
 * skipped no longer belongs to the abandoned statement, so the next
 * 'else'/'while' is a new one: a later valid while loop is parsed as a
 * statement (no cascade, real errors in it are reported) and a later
 * extra 'else' is still reported. */
TEST(parser_recovery_skipped_tail_is_not_skipped_again) {
    expect_error("do a b while (x); while (y) z = 1;", 5,
                 "expected ';' after expression");
    expect_error("do x = x + 1 while (x < 10); while (y) { z = 1; }", 13,
                 "expected ';' after expression");
    expect_error("if (a) if (b) x y else z; else w;", 16,
                 "expected ';' after expression");
    expect_error("if (x) else y;", 7, "expected statement");

    static const size_t do_offsets[] = {5, 32};
    static const char *const do_messages[] = {
        "expected ';' after expression",
        "expected expression",
    };
    expect_errors("do a b while (x); while (y) z = ;",
                  2, do_offsets, do_messages);

    static const size_t if_offsets[] = {9, 19};
    static const char *const if_messages[] = {
        "expected ';' after expression",
        "'else' without a matching 'if'",
    };
    expect_errors("if (x) a b else c; else d;",
                  2, if_offsets, if_messages);
}

/* §6.3.4 an error before 'else' abandons the whole if statement: its
 * 'else' part is skipped (still checked for real errors), never reported
 * as "'else' without a matching 'if'". Likewise the 'while (...) ;' tail
 * of a do statement whose body failed. */
TEST(parser_recovery_skips_abandoned_tails) {
    expect_error("if (x) a b; else c;", 9, "expected ';' after expression");
    expect_error("if (x +) { a; } else { b; }", 7, "expected expression");
    expect_error("if (x +) a = 1; else b = 2;", 7, "expected expression");
    expect_error("if (a) if (b) x = ; else y; else z;", 18,
                 "expected expression");
    expect_error("if (a) if (b) x = ; else { y; } else z;", 18,
                 "expected expression");
    expect_error("while (1) { if (a) b c; else d; }", 21,
                 "expected ';' after expression");
    expect_error("if (a) do x = ; while (b); else c;", 14,
                 "expected expression");
    expect_error("do if (a) x = ; else y; while (b); z;", 14,
                 "expected expression");
    expect_error("do x = ; while (y); z = 1;", 7, "expected expression");

    /* Real errors in a skipped 'else' part are still reported. */
    static const size_t else_offsets[] = {11, 22};
    static const char *const else_messages[] = {
        "expected expression",
        "expected expression",
    };
    expect_errors("if (a) q = ; else r = ;", 2, else_offsets, else_messages);

    /* Only as many 'else' as abandoned ifs are skipped. */
    static const size_t extra_offsets[] = {11, 21};
    static const char *const extra_messages[] = {
        "expected expression",
        "'else' without a matching 'if'",
    };
    expect_errors("if (a) x = ; else y; else z;", 2, extra_offsets,
                  extra_messages);

    /* An abandoned if inside a block does not swallow an 'else' after it. */
    static const size_t block_offsets[] = {13, 15};
    static const char *const block_messages[] = {
        "expected expression",
        "'else' without a matching 'if'",
    };
    expect_errors("{ if (x) y = } else z;", 2, block_offsets,
                  block_messages);

    /* A do tail without its ';' reports the real mistake. */
    static const size_t do_offsets[] = {7, 18};
    static const char *const do_messages[] = {
        "expected expression",
        "expected ';' after 'do' statement",
    };
    expect_errors("do x = ; while (y)", 2, do_offsets, do_messages);
    static const size_t do_body_offsets[] = {7, 19};
    expect_errors("do x = ; while (y) z;", 2, do_body_offsets, do_messages);
}

/* §6.3.4 step 2: an 'if' or 'do' skipped while recovering is abandoned
 * too, so its 'else' or 'while (...) ;' tail after the stop is skipped
 * instead of being reported as "'else' without a matching 'if'". */
TEST(parser_recovery_counts_skipped_ifs_and_dos) {
    expect_error("while (x +) if (y) a; else b;", 10, "expected expression");
    expect_error("if (x +) if (y) a; else b; else c;", 7,
                 "expected expression");
    expect_error("if (x) a b; else if (c) d; else e;", 9,
                 "expected ';' after expression");
    expect_error("while (x +) do a; while (b);", 10, "expected expression");
    expect_error("while (x +) if (y) { a; } else { b; }", 10,
                 "expected expression");

    /* A skipped 'if' whose 'else' was skipped as well has no tail left. */
    static const size_t offsets[] = {10, 30};
    static const char *const messages[] = {
        "expected expression",
        "'else' without a matching 'if'",
    };
    expect_errors("while (x +) if (y) a; else b; else c;", 2, offsets,
                  messages);
}

/* §6.3.4 step 2: the ')' that closes a parenthesis left open by an
 * abandoned statement belongs to it; a later complete statement ends it. */
TEST(parser_recovery_matches_unclosed_parentheses) {
    expect_error("f(a; b); c = 1;", 3, "expected ')' after arguments");
    expect_error("f(a; b)); c;", 3, "expected ')' after arguments");
    expect_error("x = (1; y); z = 2;", 6, "expected ')' after expression");
    expect_error("if (a; b) c; else d;", 5, "expected ')' after condition");
    expect_error("f(a; g(b); c = 1;", 3, "expected ')' after arguments");

    /* A ')' inside the next statement's own parentheses is its own. */
    static const size_t inner_offsets[] = {3, 10, 18};
    static const char *const inner_messages[] = {
        "expected ')' after arguments",
        "expected expression",
        "expected expression",
    };
    expect_errors("f(a; (b + )); c = ;", 3, inner_offsets, inner_messages);

    /* Parentheses never span braces. */
    static const size_t block_offsets[] = {5, 10};
    static const char *const block_messages[] = {
        "expected ')' after arguments",
        "expected ';' after expression",
    };
    expect_errors("{ f(a; } b);", 2, block_offsets, block_messages);

    /* A complete statement in between: the tail is no longer pending. */
    static const size_t else_offsets[] = {5, 13};
    static const char *const else_messages[] = {
        "expected ')' after condition",
        "'else' without a matching 'if'",
    };
    expect_errors("if (a; g(b); else d;", 2, else_offsets, else_messages);
}

/* §6.3.4 a failing condition in an abandoned do tail is reported. */
TEST(parser_recovery_reports_errors_in_do_tail_condition) {
    static const size_t offsets[] = {6, 15};
    static const char *const messages[] = {
        "expected expression",
        "expected expression",
    };
    expect_errors("do x +; while (+);", 2, offsets, messages);
}

/* §6.3.3, §6.3.4 a height error is only found once the operand, and so
 * the token after it, has been read; a lexical error in that token does
 * not hide it. */
TEST(parser_reports_height_error_before_lexical_error) {
    const size_t limit = PARSER_MAX_EXPR_HEIGHT;
    static const char *const messages[] = {
        "nesting too deep",
        "unexpected character '@'",
    };
    static const struct {
        const char *prefix;
        size_t count;
        const char *suffix;
        size_t error_offset;
        size_t lexical_offset;
    } cases[] = {
        /* binary: the 1024th '+' */
        {"x = 1", PARSER_MAX_EXPR_HEIGHT, " @", 5 + 4 * 1023 + 1,
         5 + 4 * PARSER_MAX_EXPR_HEIGHT + 1},
        /* assignment: the '=' */
        {"x = 1", PARSER_MAX_EXPR_HEIGHT - 1, " @", 2,
         5 + 4 * (PARSER_MAX_EXPR_HEIGHT - 1) + 1},
        /* unary: the '-' */
        {"-(1", PARSER_MAX_EXPR_HEIGHT - 1, ") @", 0,
         3 + 4 * (PARSER_MAX_EXPR_HEIGHT - 1) + 2},
        /* call: the '(' */
        {"f(1", PARSER_MAX_EXPR_HEIGHT - 1, ") @", 1,
         3 + 4 * (PARSER_MAX_EXPR_HEIGHT - 1) + 2},
    };

    EXPECT_EQ(1024, limit);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t length = 0;
        char *text = repeat_text(cases[i].prefix, " + 1", cases[i].count,
                                 cases[i].suffix, &length);

        EXPECT_TRUE(text != NULL);

        if (text == NULL) {
            continue;
        }

        const size_t offsets[] = {cases[i].error_offset,
                                  cases[i].lexical_offset};
        expect_errors(text, 2, offsets, messages);
        free(text);
    }

    /* Nothing at or after the bad token is reported besides it. */
    static const size_t later_offsets[] = {6};
    static const char *const later_messages[] = {
        "unsupported escape sequence",
    };
    expect_errors("x = 1 '\\q';", 1, later_offsets, later_messages);
}

/* §4.2, src/parser.c: '=' is the only right-associative operator; the
 * parser has no generic right-associative path. */
TEST(operator_table_has_only_assignment_right_associative) {
    for (int kind = TOKEN_END; kind <= TOKEN_NOT_EQUAL; ++kind) {
        const Binary_Operator *op = operator_binary((Token_Kind)kind);

        if (op != NULL && op->associativity == OPERATOR_RIGHT) {
            EXPECT_EQ(TOKEN_ASSIGN, op->kind);
        }
    }
}

/* Parses 'first' + 'link' * count + 'close' * (count + 1) and expects
 * exactly one error, a "nesting too deep" at 'error_offset'. */
static void expect_one_nesting_error(
    const char *first,
    const char *link,
    const char *close,
    size_t count,
    size_t error_offset
) {
    size_t length = 0;
    size_t close_length = 0;
    char *closing = repeat_text(close, close, count, "\n", &close_length);
    char *text =
        closing == NULL ? NULL
                        : repeat_text(first, link, count, closing, &length);

    free(closing);

    EXPECT_TRUE(text != NULL);

    if (text == NULL) {
        return;
    }

    Lil_Result result;
    EXPECT_FALSE(lil_run_source("elif.lil", text, length, &result));
    test_check_uint(1, result.errors.count, link, "error count",
                    __FILE__, __LINE__);
    EXPECT_STR_EQ("nesting too deep", result.errors.items[0].message);
    EXPECT_EQ(error_offset, result.errors.items[0].span.offset);
    lil_result_free(&result);
    free(text);
}

/* §6.3.2 a too-deep else-if chain gives one error however long it is;
 * the remaining links belong to the abandoned if and lie beyond the
 * limit too, so they are not reported (also under ASan in 'make
 * sanitize'). */
TEST(parser_too_deep_else_if_chain_reports_once) {
    static const size_t counts[] = {300, 600, 2000, 100000};
    const size_t limit = PARSER_MAX_NESTING;

    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        expect_one_nesting_error("if(a)b;", "else if(a)b;", "", counts[i],
                                 7 + 12 * (limit - 1) + 5);
        /* Blocks as branches: the then block of the 255th link opens
         * level 257. */
        expect_one_nesting_error("if(a){b;}", "else if(a){b;}", "",
                                 counts[i], 9 + 14 * (limit - 2) + 10);
        /* Each link wrapped in a block: '{' and 'if' each take a level,
         * so the '{' of the 128th link opens level 257. */
        expect_one_nesting_error("{if(a)b;", "else{if(a)b;", "}", counts[i],
                                 8 + 12 * (limit / 2 - 1) + 4);
    }

    /* A real error in the abandoned links is still reported. */
    size_t length = 0;
    char *text = repeat_text("if(a)b;", "else if(a)b;", 600,
                             "else if(a)b c;", &length);

    ASSERT_TRUE(text != NULL);

    Lil_Result result;
    EXPECT_FALSE(lil_run_source("elif.lil", text, length, &result));
    EXPECT_EQ(2, result.errors.count);
    EXPECT_STR_EQ("nesting too deep", result.errors.items[0].message);
    EXPECT_STR_EQ("expected ';' after expression",
                  result.errors.items[1].message);
    EXPECT_EQ(length - 2, result.errors.items[1].span.offset);
    lil_result_free(&result);
    free(text);
}

/* Runs 'text' and expects exactly two errors at the given offsets. */
static void expect_two_errors(
    const char *text,
    size_t length,
    size_t first_offset,
    const char *first_message,
    size_t second_offset,
    const char *second_message
) {
    Lil_Result result;

    EXPECT_TRUE(text != NULL);

    if (text == NULL) {
        return;
    }

    EXPECT_FALSE(lil_run_source("two.lil", text, length, &result));
    EXPECT_EQ(2, result.errors.count);

    if (result.errors.count == 2) {
        EXPECT_EQ(first_offset, result.errors.items[0].span.offset);
        EXPECT_STR_EQ(first_message, result.errors.items[0].message);
        EXPECT_EQ(second_offset, result.errors.items[1].span.offset);
        EXPECT_STR_EQ(second_message, result.errors.items[1].message);
    }

    lil_result_free(&result);
}

/* §6.3.4 step 3 a chain that first goes too deep inside the tail of a
 * statement abandoned for an ordinary error reports the limit once. The
 * tail restarts counting at the level of the statement list (§6.3.2). */
TEST(parser_too_deep_chain_in_recovered_tail_reports_once) {
    static const struct {
        const char *prefix;
        const char *link;
        size_t count;
        const char *suffix;
        size_t first_offset;
        const char *first_message;
        size_t refused_link;
    } cases[] = {
        /* The tail's first 'if' is level 1: the 257th link is refused. */
        {"if (x +) a; ", "else if (y) a; ", 2000, "", 7,
         "expected expression", 256},
        {"while (x +) if (y) a; ", "else if (y) a; ", 1000, "", 10,
         "expected expression", 256},
        /* Inside a block the tail starts at level 1. */
        {"{ if (x) a b; ", "else if (y) a; ", 1000, "}", 11,
         "expected ';' after expression", 255},
        /* Two levels per link: the 'while' of link 128 is level 257. */
        {"if (x +) a; ", "else while (y) if (y) a; ", 1000, "", 7,
         "expected expression", 128},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t length = 0;
        char *text = repeat_text(cases[i].prefix, cases[i].link,
                                 cases[i].count, cases[i].suffix, &length);

        expect_two_errors(text, length, cases[i].first_offset,
                          cases[i].first_message,
                          strlen(cases[i].prefix) +
                              strlen(cases[i].link) * cases[i].refused_link +
                              strlen("else "),
                          "nesting too deep");
        free(text);
    }
}

/* §6.3.2 / §6.3.4 step 3 only tails beyond the limit are quiet: a depth
 * error in the condition of a level-1 if does not hide one in its else. */
TEST(parser_reports_unrelated_depth_error_in_tail) {
    size_t condition_length = 0;
    size_t length = 0;
    char *condition = repeat_text("", "(", PARSER_MAX_NESTING + 44, "1",
                                  &condition_length);
    char *head = condition == NULL
                     ? NULL
                     : repeat_text(condition, ")", PARSER_MAX_NESTING + 44,
                                   ") a; else ", &length);
    char *blocks = repeat_text("", "{", PARSER_MAX_NESTING + 44, "",
                               &length);
    char *text = NULL;

    if (head != NULL && blocks != NULL) {
        char *opened = repeat_text("if (", head, 1, blocks, &length);

        if (opened != NULL) {
            text = repeat_text(opened, "}", PARSER_MAX_NESTING + 44, "",
                               &length);
        }

        free(opened);
    }

    /* 'if' is level 1, so the 256th '(' opens level 257; the else
     * restarts at level 0, so its 257th '{' is refused. */
    expect_two_errors(text, length, strlen("if (") + PARSER_MAX_NESTING - 1,
                      "nesting too deep",
                      strlen("if (") + strlen(head) + PARSER_MAX_NESTING,
                      "nesting too deep");
    free(text);
    free(blocks);
    free(head);
    free(condition);
}

/* §6.3.2 an error at the 102nd if of a 600-link chain: its tail restarts
 * at level 0, so the 257th if after it is too deep. */
TEST(parser_chain_error_restarts_depth_count) {
    static const char first[] = "if (x) a; ";
    static const char link[] = "else if (x) a; ";
    static const char failed[] = "else if (x +) a; ";
    size_t length = 0;
    char *before = repeat_text(first, link, 100, failed, &length);
    char *text = before == NULL
                     ? NULL
                     : repeat_text(before, link, 499, "else b;", &length);
    const size_t failed_at = strlen(first) + strlen(link) * 100;

    expect_two_errors(text, length, failed_at + strlen("else if (x +"),
                      "expected expression",
                      failed_at + strlen(failed) +
                          strlen(link) * PARSER_MAX_NESTING + strlen("else "),
                      "nesting too deep");
    free(text);
    free(before);
}

/* §6.3.4 step 3 the refused 'if' is one abandoned if, counted once: 257
 * nested ifs take 257 else tails, and the 258th 'else' is reported. */
TEST(parser_refused_if_takes_one_else) {
    static const char nested_if[] = "if (x) ";
    static const char else_tail[] = "else b; ";
    const size_t ifs = PARSER_MAX_NESTING + 1;
    size_t length = 0;
    char *head = repeat_text("", nested_if, ifs, "a; ", &length);
    char *text = head == NULL
                     ? NULL
                     : repeat_text(head, else_tail, ifs + 1, "", &length);

    expect_two_errors(text, length, strlen(nested_if) * PARSER_MAX_NESTING,
                      "nesting too deep",
                      strlen(nested_if) * ifs + strlen("a; ") +
                          strlen(else_tail) * ifs,
                      "'else' without a matching 'if'");
    free(text);
    free(head);
}

/* §6.3.4 step 2 a block parsed after an abandoned statement keeps the
 * parentheses left open outside it. */
TEST(parser_block_keeps_unclosed_parens) {
    static const size_t offsets[] = {3};
    static const char *const messages[] = {"expected ')' after arguments"};

    expect_errors("f(a; if (b) { c; } else );", 1, offsets, messages);
    expect_errors("f(a; if (b) c; else );", 1, offsets, messages);
}

/* §6.3.4 at most DIAGNOSTIC_MAX_ERRORS errors, then parsing stops. */
TEST(parser_caps_error_count) {
    size_t length = 0;
    char *text = repeat_text("", "x = ;\n", DIAGNOSTIC_MAX_ERRORS + 5, "",
                             &length);

    ASSERT_TRUE(text != NULL);

    Lil_Result result;
    EXPECT_FALSE(lil_run_source("many.lil", text, length, &result));
    EXPECT_EQ(DIAGNOSTIC_MAX_ERRORS, result.errors.count);

    for (size_t i = 0; i < result.errors.count; ++i) {
        EXPECT_EQ(6 * i + 4, result.errors.items[i].span.offset);
    }

    lil_result_free(&result);
    free(text);
}

/* §6.3.4 a lexical error is reported after earlier syntax errors and
 * stops parsing, also while skipping tokens. */
TEST(parser_stops_at_lexical_error) {
    static const size_t offsets[] = {4, 10};
    static const char *const messages[] = {
        "expected expression",
        "unexpected character '@'",
    };
    Lil_Result result;

    expect_errors("a = ; b = @; c = ;", 2, offsets, messages);

    static const size_t skip_offsets[] = {2, 4};
    static const char *const skip_messages[] = {
        "expected ';' after expression",
        "unexpected character '@'",
    };
    expect_errors("a b @ ;", 2, skip_offsets, skip_messages);

    EXPECT_FALSE(lil_run_source("bad.lil", "@", 1, &result));
    EXPECT_EQ(1, result.errors.count);
    EXPECT_EQ(DIAGNOSTIC_LEXICAL, result.errors.items[0].stage);
    lil_result_free(&result);
}

/* Builds open * count + middle + close * count + suffix. With count at
 * most PARSER_MAX_NESTING (single-level openers) it must parse; otherwise
 * it must give exactly one "nesting too deep" at 'error_offset' (§6.3.2). */
static void expect_deep_nesting_error(
    const char *open,
    const char *middle,
    const char *close,
    const char *suffix,
    size_t count,
    size_t error_offset
) {
    const size_t open_length = strlen(open);
    const size_t middle_length = strlen(middle);
    const size_t close_length = strlen(close);
    const size_t suffix_length = strlen(suffix);
    const size_t length =
        (open_length + close_length) * count + middle_length + suffix_length;
    char *text = malloc(length + 1);

    EXPECT_TRUE(text != NULL);

    if (text == NULL) {
        return;
    }

    char *cursor = text;

    for (size_t i = 0; i < count; ++i) {
        memcpy(cursor, open, open_length);
        cursor += open_length;
    }

    memcpy(cursor, middle, middle_length);
    cursor += middle_length;

    for (size_t i = 0; i < count; ++i) {
        memcpy(cursor, close, close_length);
        cursor += close_length;
    }

    memcpy(cursor, suffix, suffix_length + 1);

    Lil_Result result;
    const bool succeeded = lil_run_source("deep.lil", text, length, &result);

    if (count <= PARSER_MAX_NESTING) {
        test_check_true(succeeded, open, __FILE__, __LINE__);
    } else {
        EXPECT_FALSE(succeeded);
        test_check_uint(1, result.errors.count, open, "error count",
                        __FILE__, __LINE__);
        EXPECT_EQ(error_offset, result.diagnostic.span.offset);
        EXPECT_STR_EQ("nesting too deep", result.diagnostic.message);
    }

    lil_result_free(&result);
    free(text);
}

/* §6.3.2 10000 levels of every nesting construct give one "nesting too
 * deep" at the 257th opener instead of a stack overflow (also under ASan
 * in 'make sanitize'). */
TEST(parser_rejects_10000_levels_of_nesting) {
    const size_t deep = 10000;
    const size_t limit = PARSER_MAX_NESTING;

    expect_deep_nesting_error("(", "1", ")", ";", limit, 0);
    expect_deep_nesting_error("(", "1", ")", ";", deep, limit);
    expect_deep_nesting_error("{", "", "}", "", limit, 0);
    expect_deep_nesting_error("{", "", "}", "", deep, limit);
    expect_deep_nesting_error("-", "1", "", ";", limit, 0);
    expect_deep_nesting_error("-", "1", "", ";", deep, limit);
    expect_deep_nesting_error("~", "1", "", ";", deep, limit);
    expect_deep_nesting_error("f(", "1", ")", ";", limit, 0);
    expect_deep_nesting_error("f(", "1", ")", ";", deep, 2 * limit + 1);
    expect_deep_nesting_error("if (1) ", ";", "", "", limit, 0);
    expect_deep_nesting_error("if (1) ", ";", "", "", deep, 7 * limit);
    expect_deep_nesting_error("while (1) ", ";", "", "", deep, 10 * limit);
    expect_deep_nesting_error("do ", ";", " while (1);", "", limit, 0);
    expect_deep_nesting_error("do ", ";", " while (1);", "", deep,
                              3 * limit);
    /* Two levels per opener: '=' then '(' / '{' then 'if'. */
    expect_deep_nesting_error("x = (", "1", ")", ";", deep,
                              5 * (limit / 2) + 2);
    expect_deep_nesting_error("{ if (1) ", "x = 1;", " }", "", deep,
                              9 * (limit / 2));
}

/* §6.3.3 unary and call nodes count toward the expression height. */
TEST(parser_limits_height_of_unary_and_call) {
    EXPECT_TRUE(run_repeated("-(1", " + 1", PARSER_MAX_EXPR_HEIGHT - 2, ");",
                             0));
    EXPECT_FALSE(run_repeated("-(1", " + 1", PARSER_MAX_EXPR_HEIGHT - 1, ");",
                              0));
    EXPECT_TRUE(run_repeated("f(1", " + 1", PARSER_MAX_EXPR_HEIGHT - 2, ");",
                             0));
    EXPECT_FALSE(run_repeated("f(1", " + 1", PARSER_MAX_EXPR_HEIGHT - 1, ");",
                              1));
}

/* Round trip (plan stage 2): parse, print, reparse; the two ASTs must be
 * identical and printing must be idempotent. Returns whether 'text'
 * parsed at all. */
static bool expect_round_trip(const char *label, const char *text,
                              size_t length) {
    Lil_Result first;

    if (!lil_run_source("first.lil", text, length, &first)) {
        lil_result_free(&first);
        return false;
    }

    size_t printed_length = 0;
    char *printed =
        ast_print_program(&first.source, &first.program, &printed_length);
    char *expected = render_program(&first.source, &first.program);

    EXPECT_TRUE(printed != NULL);

    if (printed != NULL) {
        Lil_Result second;

        if (lil_run_source("second.lil", printed, printed_length, &second)) {
            char *actual = render_program(&second.source, &second.program);
            size_t reprinted_length = 0;
            char *reprinted = ast_print_program(
                &second.source, &second.program, &reprinted_length
            );

            test_check_str(expected, actual, label, "reparsed AST",
                           __FILE__, __LINE__);
            test_check_str(printed, reprinted == NULL ? "" : reprinted,
                           label, "reprinted source", __FILE__, __LINE__);
            free(actual);
            free(reprinted);
        } else {
            test_check_str("", second.diagnostic.message, label,
                           "reparse of printed source", __FILE__, __LINE__);
            fprintf(stderr, "printed source:\n%s\n", printed);
        }

        lil_result_free(&second);
    }

    free(expected);
    free(printed);
    lil_result_free(&first);
    return true;
}

/* The pretty-printer's layout (round trip relies on it). */
TEST(ast_print_formats_program) {
    const char *text =
        "x=1;if(a)if(b)y=2;else{z=-(a+b)*c;}while(1){break;}"
        "do;while(x);{}putc('\\n',010);f(a=b,~ -c);"
        "if(a){}else if(b);else{continue;}";
    const char *expected =
        "x = 1;\n"
        "if (a)\n"
        "    if (b)\n"
        "        y = 2;\n"
        "    else {\n"
        "        z = -(a + b) * c;\n"
        "    }\n"
        "while (1) {\n"
        "    break;\n"
        "}\n"
        "do\n"
        "    ;\n"
        "while (x);\n"
        "{}\n"
        "putc('\\n', 010);\n"
        "f(a = b, ~-c);\n"
        "if (a) {} else if (b)\n"
        "    ;\n"
        "else {\n"
        "    continue;\n"
        "}\n";
    Lil_Result result;

    EXPECT_TRUE(lil_run_source("print.lil", text, strlen(text), &result));

    size_t length = 0;
    char *printed = ast_print_program(&result.source, &result.program,
                                      &length);

    EXPECT_STR_EQ(expected, printed == NULL ? "" : printed);
    EXPECT_EQ(strlen(expected), length);
    free(printed);
    lil_result_free(&result);

    EXPECT_TRUE(expect_round_trip("print.lil", text, strlen(text)));
}

static char *read_whole_file(const char *path, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    Text text = {0};
    char buffer[4096];
    size_t got;

    if (file == NULL) {
        return NULL;
    }

    text_puts(&text, "");

    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text_append(&text, buffer, got);
    }

    fclose(file);
    *out_length = text.length;
    return text.data;
}

/* Round trip over every golden fixture and example that parses. Run from
 * the repository root (as 'make test' does). */
TEST(ast_print_round_trips_goldens_and_examples) {
    static const char *const directories[] = {
        "examples", "tests/golden/ast", "tests/golden/cli",
        "tests/golden/tokens",
    };
    size_t parsed = 0;

    for (size_t d = 0; d < sizeof(directories) / sizeof(directories[0]);
         ++d) {
        DIR *directory = opendir(directories[d]);

        test_check_true(directory != NULL, directories[d], __FILE__, __LINE__);

        if (directory == NULL) {
            continue;
        }

        struct dirent *entry;

        while ((entry = readdir(directory)) != NULL) {
            const size_t name_length = strlen(entry->d_name);
            char path[1024];
            size_t length = 0;

            if (name_length < 4 ||
                strcmp(entry->d_name + name_length - 4, ".lil") != 0) {
                continue;
            }

            snprintf(path, sizeof(path), "%s/%s", directories[d],
                     entry->d_name);

            char *text = read_whole_file(path, &length);
            test_check_true(text != NULL, path, __FILE__, __LINE__);

            if (text != NULL && expect_round_trip(path, text, length)) {
                ++parsed;
            }

            free(text);
        }

        closedir(directory);
    }

    /* Not vacuous: the ast/ goldens and examples all parse. */
    EXPECT_TRUE(parsed >= 15);
}

/* ---- fixed-seed random program generator ---- */

typedef struct {
    uint64_t state;
    Text text;
    bool explicit_parens;
} Generator;

static size_t gen_pick(Generator *gen, size_t count) {
    return (size_t)(test_random_next(&gen->state) % count);
}

static void gen_space(Generator *gen) {
    static const char *const spaces[] = {
        " ", " ", " ", "  ", "\n", "\t", " % comment\n", "\r\n",
    };

    text_puts(&gen->text,
              gen->explicit_parens ? " " : spaces[gen_pick(gen, 8)]);
}

static void gen_leaf(Generator *gen) {
    static const char *const leaves[] = {
        "a", "b", "x", "R0", ".t", "_q", "ifx", "Do2", "putc",
        "0", "7", "010", "42", "9223372036854775807",
        "'a'", "'\\n'", "'\\''", "'%'",
    };

    text_puts(&gen->text, leaves[gen_pick(gen, 18)]);
}

static void gen_name(Generator *gen) {
    static const char *const names[] = {"a", "b", "x", "R0", ".t", "_q"};

    text_puts(&gen->text, names[gen_pick(gen, 6)]);
}

/* 'operand' is true where an unparenthesized assignment would not parse
 * (§4.4), so assignments there are always parenthesized. In explicit mode
 * the output is exactly render_expr()'s canonical form. */
static void gen_expr(Generator *gen, int depth, bool operand) {
    const size_t choice = depth <= 0 ? 0 : gen_pick(gen, 8);
    const bool explicit_parens = gen->explicit_parens;

    switch (choice) {
        case 0:
        case 1:
            gen_leaf(gen);
            break;

        case 2: {
            const bool wrap = explicit_parens || gen_pick(gen, 4) == 0;

            text_puts(&gen->text, wrap ? "(" : "");
            text_puts(&gen->text, gen_pick(gen, 2) == 0 ? "-" : "~");
            gen_expr(gen, depth - 1, true);
            text_puts(&gen->text, wrap ? ")" : "");
            break;
        }

        case 3:
        case 4:
        case 5: {
            const bool wrap = explicit_parens || gen_pick(gen, 3) == 0;
            /* skip '=' (index 0) */
            const char *op =
                spec_binary_operators[1 + gen_pick(gen, SPEC_BINARY_COUNT - 1)]
                    .spelling;

            text_puts(&gen->text, wrap ? "(" : "");
            gen_expr(gen, depth - 1, true);
            gen_space(gen);
            text_puts(&gen->text, op);
            gen_space(gen);
            gen_expr(gen, depth - 1, true);
            text_puts(&gen->text, wrap ? ")" : "");
            break;
        }

        case 6: {
            const bool wrap = explicit_parens || operand;

            text_puts(&gen->text, wrap ? "(" : "");
            gen_name(gen);
            gen_space(gen);
            text_puts(&gen->text, "=");
            gen_space(gen);
            gen_expr(gen, depth - 1, false);
            text_puts(&gen->text, wrap ? ")" : "");
            break;
        }

        default: {
            const size_t count = gen_pick(gen, 4);

            text_puts(&gen->text, gen_pick(gen, 2) == 0 ? "putc" : "f");
            text_puts(&gen->text, "(");

            for (size_t i = 0; i < count; ++i) {
                text_puts(&gen->text, i > 0 ? ", " : "");
                gen_expr(gen, depth - 1, false);
            }

            text_puts(&gen->text, ")");
            break;
        }
    }
}

static void gen_stmt(Generator *gen, int depth) {
    const size_t choice = depth <= 0 ? gen_pick(gen, 3) : gen_pick(gen, 11);

    switch (choice) {
        case 0:
        case 1:
            gen_expr(gen, 4, false);
            text_puts(&gen->text, ";");
            break;

        case 2:
            text_puts(&gen->text, ";");
            break;

        case 3: {
            const size_t count = gen_pick(gen, 4);

            text_puts(&gen->text, "{");

            for (size_t i = 0; i < count; ++i) {
                gen_space(gen);
                gen_stmt(gen, depth - 1);
            }

            gen_space(gen);
            text_puts(&gen->text, "}");
            break;
        }

        case 4:
        case 5:
            text_puts(&gen->text, gen_pick(gen, 2) == 0 ? "if" : "IF");
            text_puts(&gen->text, " (");
            gen_expr(gen, 3, false);
            text_puts(&gen->text, ")");
            gen_space(gen);
            gen_stmt(gen, depth - 1);

            if (choice == 5) {
                gen_space(gen);
                text_puts(&gen->text, "else");
                gen_space(gen);
                gen_stmt(gen, depth - 1);
            }
            break;

        case 6:
            text_puts(&gen->text, "while (");
            gen_expr(gen, 3, false);
            text_puts(&gen->text, ")");
            gen_space(gen);
            gen_stmt(gen, depth - 1);
            break;

        case 7:
            text_puts(&gen->text, "do");
            gen_space(gen);
            gen_stmt(gen, depth - 1);
            gen_space(gen);
            text_puts(&gen->text, "while (");
            gen_expr(gen, 3, false);
            text_puts(&gen->text, ");");
            break;

        case 8:
            text_puts(&gen->text, "break;");
            break;

        case 9:
            text_puts(&gen->text, "Continue;");
            break;

        default:
            text_puts(&gen->text, "{}");
            break;
    }
}

/* §4.1, §4.2 random fully parenthesized expressions parse to exactly the
 * tree that was written. */
TEST(parser_random_parenthesized_expressions_parse_exactly) {
    Generator gen = {0x5eed1234abcdULL, {0}, true};

    for (int i = 0; i < 2000; ++i) {
        gen.text.length = 0;
        text_puts(&gen.text, "");
        gen_expr(&gen, 6, false);
        text_puts(&gen.text, ";");

        /* Explicit mode writes render_expr()'s canonical form. */
        expect_parses_as(gen.text.data, gen.text.data);
    }

    free(gen.text.data);
}

/* Plan stage 2: round trip over a fixed-seed random program generator. */
TEST(ast_print_round_trips_random_programs) {
    Generator gen = {0xc0ffee5eedULL, {0}, false};

    for (int i = 0; i < 1500; ++i) {
        const size_t count = 1 + gen_pick(&gen, 5);

        gen.text.length = 0;
        text_puts(&gen.text, "");

        for (size_t s = 0; s < count; ++s) {
            gen_stmt(&gen, 4);
            gen_space(&gen);
        }

        char label[32];
        snprintf(label, sizeof(label), "random #%d", i);

        if (!expect_round_trip(label, gen.text.data, gen.text.length)) {
            test_check_str("", gen.text.data, label, "generated program",
                           __FILE__, __LINE__);
        }
    }

    free(gen.text.data);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        test_set_filter(argv[1]);
    }

    RUN_TEST(lexer_tokens_refer_to_owned_source_by_span);
    RUN_TEST(lexer_reports_structured_diagnostic);
    RUN_TEST(lexer_reports_end_position_after_ignored_input);
    RUN_TEST(parser_builds_precedence_ast_at_public_seam);
    RUN_TEST(parser_builds_right_associative_assignment);
    RUN_TEST(run_source_owns_complete_program);
    RUN_TEST(run_source_returns_lexical_diagnostic);
    RUN_TEST(run_source_returns_syntax_diagnostic);
    RUN_TEST(run_source_rejects_invalid_assignment_target);
    RUN_TEST(run_source_accepts_empty_program);
    RUN_TEST(result_free_is_repeatable_after_success);
    RUN_TEST(result_free_is_repeatable_after_failure);
    RUN_TEST(lexer_recognizes_every_token_kind);
    RUN_TEST(lexer_accepts_character_literals);
    RUN_TEST(lexer_reports_unterminated_character_literal);
    RUN_TEST(lexer_reports_character_literal_errors);
    RUN_TEST(lexer_reports_unsupported_escape_before_unterminated);
    RUN_TEST(lexer_comment_does_not_end_at_bare_cr);
    RUN_TEST(lexer_identifiers_are_ascii_regardless_of_locale);
    RUN_TEST(lexer_escapes_unprintable_unexpected_character);
    RUN_TEST(lexer_computes_character_literal_values);
    RUN_TEST(lexer_computes_integer_literal_values);
    RUN_TEST(lexer_reports_integer_literal_errors);
    RUN_TEST(lexer_applies_longest_match);
    RUN_TEST(lexer_property_spans_partition_random_input);
    RUN_TEST(token_kind_name_covers_every_kind);
    RUN_TEST(run_source_requires_semicolon);
    RUN_TEST(parser_limits_assignment_nesting);
    RUN_TEST(parser_rejects_huge_assignment_chain_without_crashing);
    RUN_TEST(parser_limits_expression_height);
    RUN_TEST(parser_rejects_huge_operator_chain_without_crashing);
    RUN_TEST(parser_handles_many_statements);
    RUN_TEST(parser_precedence_examples);
    RUN_TEST(parser_binary_operator_pairs_follow_precedence_table);
    RUN_TEST(parser_unary_operators_bind_tighter_than_binary);
    RUN_TEST(parser_parentheses_group);
    RUN_TEST(parser_parses_calls);
    RUN_TEST(parser_parses_statements);
    RUN_TEST(parser_binds_dangling_else_to_nearest_if);
    RUN_TEST(parser_records_node_spans);
    RUN_TEST(parser_reports_each_syntax_error);
    RUN_TEST(parser_recovers_and_reports_independent_errors);
    RUN_TEST(parser_recovery_does_not_cascade);
    RUN_TEST(parser_recovery_skips_abandoned_tails);
    RUN_TEST(parser_recovery_skipped_tail_is_not_skipped_again);
    RUN_TEST(parser_recovery_counts_skipped_ifs_and_dos);
    RUN_TEST(parser_recovery_matches_unclosed_parentheses);
    RUN_TEST(parser_recovery_reports_errors_in_do_tail_condition);
    RUN_TEST(parser_reports_height_error_before_lexical_error);
    RUN_TEST(operator_table_has_only_assignment_right_associative);
    RUN_TEST(parser_too_deep_else_if_chain_reports_once);
    RUN_TEST(parser_too_deep_chain_in_recovered_tail_reports_once);
    RUN_TEST(parser_reports_unrelated_depth_error_in_tail);
    RUN_TEST(parser_chain_error_restarts_depth_count);
    RUN_TEST(parser_refused_if_takes_one_else);
    RUN_TEST(parser_block_keeps_unclosed_parens);
    RUN_TEST(parser_caps_error_count);
    RUN_TEST(parser_stops_at_lexical_error);
    RUN_TEST(parser_rejects_10000_levels_of_nesting);
    RUN_TEST(parser_limits_height_of_unary_and_call);
    RUN_TEST(ast_print_formats_program);
    RUN_TEST(ast_print_round_trips_goldens_and_examples);
    RUN_TEST(parser_random_parenthesized_expressions_parse_exactly);
    RUN_TEST(ast_print_round_trips_random_programs);
    return test_finish();
}
