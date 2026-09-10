#include "ast_debug.h"

static void print_token(
    FILE *stream,
    const Source *source,
    Token token
) {
    fputc('"', stream);
    fwrite(
        source_text_at(source, token.span),
        1,
        token.span.length,
        stream
    );
    fputc('"', stream);
}

static void print_expression(
    FILE *stream,
    const Source *source,
    const Expr *expression,
    size_t depth
) {
    for (size_t i = 0; i < depth; ++i) {
        fputs("  ", stream);
    }

    switch (expression->kind) {
        case EXPR_VARIABLE:
            fputs("Variable ", stream);
            print_token(stream, source, expression->as.variable);
            fputc('\n', stream);
            break;

        case EXPR_LITERAL:
            fputs("Literal ", stream);
            print_token(stream, source, expression->as.literal);
            fputc('\n', stream);
            break;

        case EXPR_BINARY:
            fputs("Binary ", stream);
            print_token(stream, source, expression->as.binary.operator_token);
            fputc('\n', stream);
            print_expression(stream, source, expression->as.binary.left, depth + 1);
            print_expression(stream, source, expression->as.binary.right, depth + 1);
            break;

        case EXPR_ASSIGNMENT:
            fputs("Assignment ", stream);
            print_token(stream, source, expression->as.assignment.name);
            fputc('\n', stream);
            print_expression(stream, source, expression->as.assignment.value, depth + 1);
            break;
    }
}

void ast_debug_print_program(
    FILE *stream,
    const Source *source,
    const Program *program
) {
    fprintf(stream, "Program (%zu expressions)\n", program->expression_count);

    for (size_t i = 0; i < program->expression_count; ++i) {
        fprintf(stream, "[%zu]\n", i);
        print_expression(stream, source, program->expressions[i], 1);
    }
}
