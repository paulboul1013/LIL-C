#include "ast_print.h"

#include "operator.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Layout: one statement per line, four spaces per level. A block body
 * stays on the line of its 'if'/'while'/'do'/'else' ("if (c) {"); any
 * other body goes on its own, more indented line. "else if" chains stay
 * flat. Recursion depth is bounded by the parser's §6.3 limits.
 */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;            /* out of memory; the output is discarded */
} Printer;

static void print_bytes(Printer *printer, const char *bytes, size_t length) {
    if (printer->failed) {
        return;
    }

    if (length >= printer->capacity - printer->length) {
        size_t capacity = printer->capacity;

        while (length >= capacity - printer->length) {
            if (capacity > SIZE_MAX / 2) {
                printer->failed = true;
                return;
            }

            capacity *= 2;
        }

        char *grown = realloc(printer->data, capacity);

        if (grown == NULL) {
            printer->failed = true;
            return;
        }

        printer->data = grown;
        printer->capacity = capacity;
    }

    memcpy(printer->data + printer->length, bytes, length);
    printer->length += length;
    printer->data[printer->length] = '\0';
}

static void print_text(Printer *printer, const char *text) {
    print_bytes(printer, text, strlen(text));
}

/* Tokens are printed with their original spelling. */
static void print_token(Printer *printer, const Source *source, Token token) {
    print_bytes(printer, source_text_at(source, token.span), token.span.length);
}

static void print_indent(Printer *printer, size_t level) {
    for (size_t i = 0; i < level; ++i) {
        print_text(printer, "    ");
    }
}

static int expr_precedence(const Expr *expression) {
    switch (expression->kind) {
        case EXPR_UNARY:
            return OPERATOR_PRECEDENCE_UNARY;

        case EXPR_BINARY:
            return operator_binary(
                expression->as.binary.operator_token.kind
            )->precedence;

        case EXPR_ASSIGNMENT:
            return OPERATOR_PRECEDENCE_ASSIGNMENT;

        case EXPR_VARIABLE:
        case EXPR_LITERAL:
        case EXPR_CALL:
            break;
    }

    return OPERATOR_PRECEDENCE_PRIMARY;
}

/* Prints 'expression', parenthesized when it binds looser than
 * 'min_precedence' requires (§4.2). */
static void print_expr(
    Printer *printer,
    const Source *source,
    const Expr *expression,
    int min_precedence
) {
    const int precedence = expr_precedence(expression);
    const bool parenthesize = precedence < min_precedence;

    if (parenthesize) {
        print_text(printer, "(");
    }

    switch (expression->kind) {
        case EXPR_VARIABLE:
            print_token(printer, source, expression->as.variable);
            break;

        case EXPR_LITERAL:
            print_token(printer, source, expression->as.literal);
            break;

        case EXPR_UNARY:
            print_token(printer, source, expression->as.unary.operator_token);
            print_expr(printer, source, expression->as.unary.operand,
                       OPERATOR_PRECEDENCE_UNARY);
            break;

        case EXPR_BINARY: {
            const Binary_Operator *op =
                operator_binary(expression->as.binary.operator_token.kind);
            const bool left_associative =
                op->associativity == OPERATOR_LEFT;

            print_expr(printer, source, expression->as.binary.left,
                       left_associative ? precedence : precedence + 1);
            print_text(printer, " ");
            print_token(printer, source,
                        expression->as.binary.operator_token);
            print_text(printer, " ");
            print_expr(printer, source, expression->as.binary.right,
                       left_associative ? precedence + 1 : precedence);
            break;
        }

        case EXPR_ASSIGNMENT:
            print_token(printer, source, expression->as.assignment.name);
            print_text(printer, " = ");
            print_expr(printer, source, expression->as.assignment.value,
                       OPERATOR_PRECEDENCE_ASSIGNMENT);
            break;

        case EXPR_CALL:
            print_token(printer, source, expression->as.call.name);
            print_text(printer, "(");

            for (size_t i = 0; i < expression->as.call.argument_count; ++i) {
                if (i > 0) {
                    print_text(printer, ", ");
                }

                print_expr(printer, source, expression->as.call.arguments[i],
                           OPERATOR_PRECEDENCE_ASSIGNMENT);
            }

            print_text(printer, ")");
            break;
    }

    if (parenthesize) {
        print_text(printer, ")");
    }
}

static void print_stmt(
    Printer *printer,
    const Source *source,
    const Stmt *statement,
    size_t level
);

/* The body of if/else/while/do: a block on the same line, anything else
 * on the next line one level deeper. Leaves the cursor after the body. */
static void print_body(
    Printer *printer,
    const Source *source,
    const Stmt *body,
    size_t level
) {
    if (body->kind == STMT_BLOCK) {
        print_text(printer, " ");
        print_stmt(printer, source, body, level);
        return;
    }

    print_text(printer, "\n");
    print_indent(printer, level + 1);
    print_stmt(printer, source, body, level + 1);
}

static void print_condition(
    Printer *printer,
    const Source *source,
    const char *keyword,
    const Expr *condition
) {
    print_text(printer, keyword);
    print_text(printer, " (");
    print_expr(printer, source, condition, OPERATOR_PRECEDENCE_ASSIGNMENT);
    print_text(printer, ")");
}

/* Prints a statement starting at the cursor (already indented); does not
 * print the final newline. */
static void print_stmt(
    Printer *printer,
    const Source *source,
    const Stmt *statement,
    size_t level
) {
    switch (statement->kind) {
        case STMT_EXPRESSION:
            print_expr(printer, source, statement->as.expression,
                       OPERATOR_PRECEDENCE_ASSIGNMENT);
            print_text(printer, ";");
            break;

        case STMT_EMPTY:
            print_text(printer, ";");
            break;

        case STMT_BLOCK:
            print_text(printer, "{");

            for (size_t i = 0; i < statement->as.block.statement_count; ++i) {
                print_text(printer, "\n");
                print_indent(printer, level + 1);
                print_stmt(printer, source, statement->as.block.statements[i],
                           level + 1);
            }

            if (statement->as.block.statement_count > 0) {
                print_text(printer, "\n");
                print_indent(printer, level);
            }

            print_text(printer, "}");
            break;

        case STMT_IF: {
            const Stmt *then_branch = statement->as.if_statement.then_branch;
            const Stmt *else_branch = statement->as.if_statement.else_branch;

            print_condition(printer, source, "if",
                            statement->as.if_statement.condition);
            print_body(printer, source, then_branch, level);

            if (else_branch == NULL) {
                break;
            }

            if (then_branch->kind == STMT_BLOCK) {
                print_text(printer, " else");
            } else {
                print_text(printer, "\n");
                print_indent(printer, level);
                print_text(printer, "else");
            }

            if (else_branch->kind == STMT_IF) {
                print_text(printer, " ");
                print_stmt(printer, source, else_branch, level);
            } else {
                print_body(printer, source, else_branch, level);
            }
            break;
        }

        case STMT_WHILE:
            print_condition(printer, source, "while",
                            statement->as.while_statement.condition);
            print_body(printer, source, statement->as.while_statement.body,
                       level);
            break;

        case STMT_DO: {
            const Stmt *body = statement->as.do_statement.body;

            print_text(printer, "do");
            print_body(printer, source, body, level);

            if (body->kind == STMT_BLOCK) {
                print_text(printer, " ");
            } else {
                print_text(printer, "\n");
                print_indent(printer, level);
            }

            print_condition(printer, source, "while",
                            statement->as.do_statement.condition);
            print_text(printer, ";");
            break;
        }

        case STMT_BREAK:
            print_text(printer, "break;");
            break;

        case STMT_CONTINUE:
            print_text(printer, "continue;");
            break;
    }
}

char *ast_print_program(
    const Source *source,
    const Program *program,
    size_t *out_length
) {
    Printer printer = {0};

    printer.capacity = 256;
    printer.data = malloc(printer.capacity);

    if (printer.data == NULL) {
        return NULL;
    }

    printer.data[0] = '\0';

    for (size_t i = 0; i < program->statement_count; ++i) {
        print_stmt(&printer, source, program->statements[i], 0);
        print_text(&printer, "\n");
    }

    if (printer.failed) {
        free(printer.data);
        return NULL;
    }

    *out_length = printer.length;
    return printer.data;
}
