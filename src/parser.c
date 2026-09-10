#include "parser.h"

#include <stdlib.h>

struct Parser {
    Lexer *lexer;
    Token current;
    Diagnostic diagnostic;
};

static void parser_set_error(
    Parser *parser,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *message
) {
    if (!diagnostic_has_error(&parser->diagnostic)) {
        diagnostic_set(
            &parser->diagnostic,
            stage,
            span,
            "%s",
            message
        );
    }
}

static bool parser_advance(Parser *parser) {
    parser->current = lexer_next_token(parser->lexer);

    if (parser->current.kind == TOKEN_INVALID) {
        parser->diagnostic = *lexer_diagnostic(parser->lexer);
        return false;
    }

    return true;
}

Parser *parser_create(Lexer *lexer) {
    Parser *parser = malloc(sizeof(*parser));

    if (parser == NULL) {
        return NULL;
    }

    parser->lexer = lexer;
    diagnostic_clear(&parser->diagnostic);
    parser_advance(parser);
    return parser;
}

void parser_destroy(Parser *parser) {
    free(parser);
}

static Source_Span source_span_through(
    Source_Span first,
    Source_Span last
) {
    Source_Span span = first;
    span.length = last.offset + last.length - first.offset;
    return span;
}

static Expr *parser_parse_primary(Parser *parser) {
    const bool is_variable = parser->current.kind == TOKEN_IDENTIFIER;
    const bool is_literal = parser->current.kind == TOKEN_NUMBER ||
                            parser->current.kind == TOKEN_CHARACTER;

    if (!is_variable && !is_literal) {
        parser_set_error(
            parser,
            DIAGNOSTIC_SYNTAX,
            parser->current.span,
            "expected expression"
        );
        return NULL;
    }

    Expr *expression = malloc(sizeof(*expression));

    if (expression == NULL) {
        parser_set_error(
            parser,
            DIAGNOSTIC_INTERNAL,
            parser->current.span,
            "out of memory while parsing expression"
        );
        return NULL;
    }

    if (is_variable) {
        expression->kind = EXPR_VARIABLE;
        expression->as.variable = parser->current;
    } else {
        expression->kind = EXPR_LITERAL;
        expression->as.literal = parser->current;
    }

    expression->span = parser->current.span;

    if (!parser_advance(parser)) {
        free(expression);
        return NULL;
    }

    return expression;
}

static Expr *parser_make_binary(
    Parser *parser,
    Expr *left,
    Token operator_token,
    Expr *right
) {
    Expr *expression = malloc(sizeof(*expression));

    if (expression == NULL) {
        parser_set_error(
            parser,
            DIAGNOSTIC_INTERNAL,
            operator_token.span,
            "out of memory while parsing expression"
        );
        expr_free(left);
        expr_free(right);
        return NULL;
    }

    expression->kind = EXPR_BINARY;
    expression->span = source_span_through(left->span, right->span);
    expression->as.binary.left = left;
    expression->as.binary.operator_token = operator_token;
    expression->as.binary.right = right;
    return expression;
}

static Expr *parser_parse_multiplicative(Parser *parser) {
    Expr *left = parser_parse_primary(parser);

    while (left != NULL &&
           (parser->current.kind == TOKEN_STAR ||
            parser->current.kind == TOKEN_SLASH)) {
        const Token operator_token = parser->current;

        if (!parser_advance(parser)) {
            expr_free(left);
            return NULL;
        }

        Expr *right = parser_parse_primary(parser);

        if (right == NULL) {
            expr_free(left);
            return NULL;
        }

        left = parser_make_binary(parser, left, operator_token, right);
    }

    return left;
}

static Expr *parser_parse_additive(Parser *parser) {
    Expr *left = parser_parse_multiplicative(parser);

    while (left != NULL &&
           (parser->current.kind == TOKEN_PLUS ||
            parser->current.kind == TOKEN_MINUS)) {
        const Token operator_token = parser->current;

        if (!parser_advance(parser)) {
            expr_free(left);
            return NULL;
        }

        Expr *right = parser_parse_multiplicative(parser);

        if (right == NULL) {
            expr_free(left);
            return NULL;
        }

        left = parser_make_binary(parser, left, operator_token, right);
    }

    return left;
}

static Expr *parser_parse_expression(Parser *parser) {
    Expr *left = parser_parse_additive(parser);

    if (left == NULL || parser->current.kind != TOKEN_ASSIGN) {
        return left;
    }

    const Token equals_token = parser->current;

    if (left->kind != EXPR_VARIABLE) {
        parser_set_error(
            parser,
            DIAGNOSTIC_SYNTAX,
            equals_token.span,
            "invalid assignment target"
        );
        expr_free(left);
        return NULL;
    }

    const Token name = left->as.variable;

    if (!parser_advance(parser)) {
        expr_free(left);
        return NULL;
    }

    Expr *value = parser_parse_expression(parser);

    if (value == NULL) {
        expr_free(left);
        return NULL;
    }

    left->kind = EXPR_ASSIGNMENT;
    left->span = source_span_through(name.span, value->span);
    left->as.assignment.name = name;
    left->as.assignment.equals_token = equals_token;
    left->as.assignment.value = value;
    return left;
}

bool parser_parse_program(Parser *parser, Program *out) {
    out->expressions = NULL;
    out->expression_count = 0;

    if (diagnostic_has_error(&parser->diagnostic)) {
        return false;
    }

    while (parser->current.kind != TOKEN_END) {
        Expr *expression = parser_parse_expression(parser);

        if (expression == NULL) {
            program_free(out);
            return false;
        }

        if (parser->current.kind != TOKEN_SEMICOLON) {
            parser_set_error(
                parser,
                DIAGNOSTIC_SYNTAX,
                parser->current.span,
                "expected ';' after expression"
            );
            expr_free(expression);
            program_free(out);
            return false;
        }

        if (!parser_advance(parser)) {
            expr_free(expression);
            program_free(out);
            return false;
        }

        Expr **expressions = realloc(
            out->expressions,
            (out->expression_count + 1) * sizeof(*out->expressions)
        );

        if (expressions == NULL) {
            parser_set_error(
                parser,
                DIAGNOSTIC_INTERNAL,
                parser->current.span,
                "out of memory while parsing program"
            );
            expr_free(expression);
            program_free(out);
            return false;
        }

        out->expressions = expressions;
        out->expressions[out->expression_count] = expression;
        ++out->expression_count;
    }

    return true;
}

const Diagnostic *parser_diagnostic(const Parser *parser) {
    return &parser->diagnostic;
}
