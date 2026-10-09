#include "ast_debug.h"

#include <inttypes.h>

/*
 * --dump-ast (docs/spec.md §6.4.3): one S-expression node per line,
 * children indented by two spaces, closing parentheses at the end of the
 * last child's line. Every node carries "@line:column+length". Recursion
 * depth is bounded by the parser's §6.3 limits.
 */

static void print_open(FILE *stream, size_t depth, const char *kind) {
    if (depth > 0) {
        fputc('\n', stream);
    }

    for (size_t i = 0; i < depth; ++i) {
        fputs("  ", stream);
    }

    fputc('(', stream);
    fputs(kind, stream);
}

static void print_span(FILE *stream, Source_Span span) {
    fprintf(stream, " @%zu:%zu+%zu", span.line, span.column, span.length);
}

/* Identifiers and operators are plain ASCII (§2.4, §2.8). */
static void print_spelling(FILE *stream, const Source *source, Token token) {
    fputc(' ', stream);
    fwrite(source_text_at(source, token.span), 1, token.span.length, stream);
}

/* The symbol id assigned by semantic analysis (§6.4.3); "#?" when the
 * name was never resolved (SYMBOL_NONE). */
static void print_symbol(FILE *stream, size_t symbol) {
    if (symbol == SYMBOL_NONE) {
        fputs(" #?", stream);
        return;
    }

    fprintf(stream, " #%zu", symbol);
}

static void print_expression(
    FILE *stream,
    const Source *source,
    const Expr *expression,
    size_t depth
) {
    switch (expression->kind) {
        case EXPR_VARIABLE:
            print_open(stream, depth, "name");
            print_spelling(stream, source, expression->as.variable);
            print_symbol(stream, expression->symbol);
            print_span(stream, expression->span);
            break;

        case EXPR_LITERAL:
            print_open(
                stream,
                depth,
                expression->as.literal.kind == TOKEN_CHARACTER ? "char" : "int"
            );
            fprintf(stream, " %" PRId64, expression->as.literal.value);
            print_span(stream, expression->span);
            break;

        case EXPR_UNARY:
            print_open(stream, depth, "unary");
            print_spelling(stream, source, expression->as.unary.operator_token);
            print_span(stream, expression->span);
            print_expression(stream, source, expression->as.unary.operand,
                             depth + 1);
            break;

        case EXPR_BINARY:
            print_open(stream, depth, "binary");
            print_spelling(stream, source,
                           expression->as.binary.operator_token);
            print_span(stream, expression->span);
            print_expression(stream, source, expression->as.binary.left,
                             depth + 1);
            print_expression(stream, source, expression->as.binary.right,
                             depth + 1);
            break;

        case EXPR_ASSIGNMENT:
            print_open(stream, depth, "assign");
            print_spelling(stream, source, expression->as.assignment.name);
            print_symbol(stream, expression->symbol);
            print_span(stream, expression->span);
            print_expression(stream, source, expression->as.assignment.value,
                             depth + 1);
            break;

        case EXPR_CALL:
            print_open(stream, depth, "call");
            print_spelling(stream, source, expression->as.call.name);
            print_span(stream, expression->span);

            for (size_t i = 0; i < expression->as.call.argument_count; ++i) {
                print_expression(stream, source,
                                 expression->as.call.arguments[i], depth + 1);
            }
            break;
    }

    fputc(')', stream);
}

static void print_statement(
    FILE *stream,
    const Source *source,
    const Stmt *statement,
    size_t depth
) {
    switch (statement->kind) {
        case STMT_EXPRESSION:
            print_open(stream, depth, "expr");
            print_span(stream, statement->span);
            print_expression(stream, source, statement->as.expression,
                             depth + 1);
            break;

        case STMT_EMPTY:
            print_open(stream, depth, "empty");
            print_span(stream, statement->span);
            break;

        case STMT_BLOCK:
            print_open(stream, depth, "block");
            print_span(stream, statement->span);

            for (size_t i = 0; i < statement->as.block.statement_count; ++i) {
                print_statement(stream, source,
                                statement->as.block.statements[i], depth + 1);
            }
            break;

        case STMT_IF:
            print_open(stream, depth, "if");
            print_span(stream, statement->span);
            print_expression(stream, source,
                             statement->as.if_statement.condition, depth + 1);
            print_statement(stream, source,
                            statement->as.if_statement.then_branch, depth + 1);

            if (statement->as.if_statement.else_branch != NULL) {
                print_statement(stream, source,
                                statement->as.if_statement.else_branch,
                                depth + 1);
            }
            break;

        case STMT_WHILE:
            print_open(stream, depth, "while");
            print_span(stream, statement->span);
            print_expression(stream, source,
                             statement->as.while_statement.condition,
                             depth + 1);
            print_statement(stream, source, statement->as.while_statement.body,
                            depth + 1);
            break;

        case STMT_DO:
            print_open(stream, depth, "do");
            print_span(stream, statement->span);
            print_statement(stream, source, statement->as.do_statement.body,
                            depth + 1);
            print_expression(stream, source,
                             statement->as.do_statement.condition, depth + 1);
            break;

        case STMT_BREAK:
            print_open(stream, depth, "break");
            print_span(stream, statement->span);
            break;

        case STMT_CONTINUE:
            print_open(stream, depth, "continue");
            print_span(stream, statement->span);
            break;
    }

    fputc(')', stream);
}

void ast_debug_print_program(
    FILE *stream,
    const Source *source,
    const Program *program
) {
    print_open(stream, 0, "program");
    print_span(stream, program->span);

    for (size_t i = 0; i < program->statement_count; ++i) {
        print_statement(stream, source, program->statements[i], 1);
    }

    fputs(")\n", stream);
}
