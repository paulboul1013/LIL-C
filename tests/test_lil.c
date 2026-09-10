#include "lexer.h"
#include "lil.h"
#include "parser.h"
#include "source.h"

#include <assert.h>
#include <string.h>

static bool token_equals(
    const Source *source,
    Token token,
    const char *expected
) {
    const size_t length = strlen(expected);

    return token.span.length == length &&
           memcmp(source_text_at(source, token.span), expected, length) == 0;
}

static void lexer_tokens_refer_to_owned_source_by_span(void) {
    char input[] = "name";
    Source source;

    assert(source_init(&source, "memory.lil", input, strlen(input)));
    input[0] = 'X';

    Lexer lexer;
    lexer_init(&lexer, &source);

    const Token token = lexer_next_token(&lexer);

    assert(token.kind == TOKEN_IDENTIFIER);
    assert(token.span.offset == 0);
    assert(token.span.length == 4);
    assert(token.span.line == 1);
    assert(token.span.column == 1);
    assert(token_equals(&source, token, "name"));

    source_free(&source);
}

static void lexer_reports_structured_diagnostic(void) {
    Source source;
    assert(source_init(&source, "bad.lil", "\n @", 3));

    Lexer lexer;
    lexer_init(&lexer, &source);

    const Token token = lexer_next_token(&lexer);
    const Diagnostic *diagnostic = lexer_diagnostic(&lexer);

    assert(token.kind == TOKEN_INVALID);
    assert(diagnostic->stage == DIAGNOSTIC_LEXICAL);
    assert(diagnostic->span.offset == 2);
    assert(diagnostic->span.line == 2);
    assert(diagnostic->span.column == 2);
    assert(strcmp(diagnostic->message, "unexpected character '@'") == 0);

    source_free(&source);
}

static void lexer_reports_end_position_after_ignored_input(void) {
    const char input[] = "\n  x\n\t+ \n";
    Source source;
    Lexer lexer;
    Token token;

    assert(source_init(&source, "memory.lil", input, strlen(input)));
    lexer_init(&lexer, &source);

    token = lexer_next_token(&lexer);
    assert(token.kind == TOKEN_IDENTIFIER);
    assert(token.span.line == 2);
    assert(token.span.column == 3);

    token = lexer_next_token(&lexer);
    assert(token.kind == TOKEN_PLUS);
    assert(token.span.line == 3);
    assert(token.span.column == 2);

    token = lexer_next_token(&lexer);
    assert(token.kind == TOKEN_END);
    assert(token.span.offset == strlen(input));
    assert(token.span.length == 0);
    assert(token.span.line == 4);
    assert(token.span.column == 1);

    source_free(&source);
}

static void parser_builds_precedence_ast_at_public_seam(void) {
    Source source;
    assert(source_init(&source, "memory.lil", "a + b * c;", 10));

    Lexer lexer;
    lexer_init(&lexer, &source);
    Parser *parser = parser_create(&lexer);
    assert(parser != NULL);

    Program program;
    assert(parser_parse_program(parser, &program));
    assert(program.expression_count == 1);

    const Expr *addition = program.expressions[0];
    assert(addition->kind == EXPR_BINARY);
    assert(addition->span.offset == 0);
    assert(addition->span.length == 9);
    assert(addition->as.binary.operator_token.kind == TOKEN_PLUS);
    assert(token_equals(&source, addition->as.binary.left->as.variable, "a"));

    const Expr *multiplication = addition->as.binary.right;
    assert(multiplication->kind == EXPR_BINARY);
    assert(multiplication->span.offset == 4);
    assert(multiplication->span.length == 5);
    assert(multiplication->as.binary.operator_token.kind == TOKEN_STAR);
    assert(token_equals(&source, multiplication->as.binary.left->as.variable, "b"));
    assert(token_equals(&source, multiplication->as.binary.right->as.variable, "c"));

    program_free(&program);
    parser_destroy(parser);
    source_free(&source);
}

static void parser_builds_right_associative_assignment(void) {
    Source source;
    assert(source_init(&source, "memory.lil", "a = b = 1;", 10));

    Lexer lexer;
    lexer_init(&lexer, &source);
    Parser *parser = parser_create(&lexer);
    assert(parser != NULL);

    Program program;
    assert(parser_parse_program(parser, &program));

    const Expr *outer = program.expressions[0];
    assert(outer->kind == EXPR_ASSIGNMENT);
    assert(outer->span.offset == 0);
    assert(outer->span.length == 9);
    assert(token_equals(&source, outer->as.assignment.name, "a"));

    const Expr *inner = outer->as.assignment.value;
    assert(inner->kind == EXPR_ASSIGNMENT);
    assert(inner->span.offset == 4);
    assert(inner->span.length == 5);
    assert(token_equals(&source, inner->as.assignment.name, "b"));
    assert(inner->as.assignment.value->kind == EXPR_LITERAL);
    assert(token_equals(
        &source,
        inner->as.assignment.value->as.literal,
        "1"
    ));

    program_free(&program);
    parser_destroy(parser);
    source_free(&source);
}

static void run_source_owns_complete_program(void) {
    Lil_Result result;

    assert(lil_run_source("memory.lil", "x + 1; y = 2;", 13, &result));
    assert(result.has_program);
    assert(result.program.expression_count == 2);
    assert(result.program.expressions[0]->kind == EXPR_BINARY);
    assert(result.program.expressions[1]->kind == EXPR_ASSIGNMENT);
    assert(token_equals(
        &result.source,
        result.program.expressions[1]->as.assignment.name,
        "y"
    ));

    lil_result_free(&result);
}

static void run_source_returns_lexical_diagnostic(void) {
    Lil_Result result;

    assert(!lil_run_source("bad.lil", "@", 1, &result));
    assert(!result.has_program);
    assert(result.diagnostic.stage == DIAGNOSTIC_LEXICAL);
    assert(result.diagnostic.span.line == 1);
    assert(result.diagnostic.span.column == 1);
    assert(strcmp(
        result.diagnostic.message,
        "unexpected character '@'"
    ) == 0);

    lil_result_free(&result);
}

static void run_source_returns_syntax_diagnostic(void) {
    Lil_Result result;

    assert(!lil_run_source("bad.lil", "x + ;", 5, &result));
    assert(result.diagnostic.stage == DIAGNOSTIC_SYNTAX);
    assert(result.diagnostic.span.offset == 4);
    assert(result.diagnostic.span.line == 1);
    assert(result.diagnostic.span.column == 5);
    assert(strcmp(result.diagnostic.message, "expected expression") == 0);

    lil_result_free(&result);
}

static void run_source_rejects_invalid_assignment_target(void) {
    Lil_Result result;

    assert(!lil_run_source("bad.lil", "x + y = z;", 10, &result));
    assert(result.diagnostic.stage == DIAGNOSTIC_SYNTAX);
    assert(result.diagnostic.span.offset == 6);
    assert(strcmp(result.diagnostic.message, "invalid assignment target") == 0);

    lil_result_free(&result);
}

static void run_source_accepts_empty_program(void) {
    Lil_Result result;

    assert(lil_run_source("empty.lil", "", 0, &result));
    assert(result.has_program);
    assert(result.program.expression_count == 0);

    lil_result_free(&result);
}

static void result_free_is_repeatable_after_success(void) {
    Lil_Result result;

    assert(lil_run_source("memory.lil", "a = 1; b = 2;", 13, &result));
    lil_result_free(&result);
    lil_result_free(&result);

    assert(!result.has_program);
    assert(result.source.path == NULL);
    assert(result.source.text == NULL);
    assert(result.program.expressions == NULL);
    assert(result.program.expression_count == 0);
    assert(!diagnostic_has_error(&result.diagnostic));
}

static void result_free_is_repeatable_after_failure(void) {
    Lil_Result result;

    assert(!lil_run_source("bad.lil", "a = 1; @", 8, &result));
    lil_result_free(&result);
    lil_result_free(&result);

    assert(!result.has_program);
    assert(result.source.path == NULL);
    assert(result.source.text == NULL);
    assert(result.program.expressions == NULL);
    assert(result.program.expression_count == 0);
    assert(!diagnostic_has_error(&result.diagnostic));
}

int main(void) {
    lexer_tokens_refer_to_owned_source_by_span();
    lexer_reports_structured_diagnostic();
    lexer_reports_end_position_after_ignored_input();
    parser_builds_precedence_ast_at_public_seam();
    parser_builds_right_associative_assignment();
    run_source_owns_complete_program();
    run_source_returns_lexical_diagnostic();
    run_source_returns_syntax_diagnostic();
    run_source_rejects_invalid_assignment_target();
    run_source_accepts_empty_program();
    result_free_is_repeatable_after_success();
    result_free_is_repeatable_after_failure();
    return 0;
}
