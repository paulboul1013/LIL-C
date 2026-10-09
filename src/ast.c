#include "ast.h"

#include <stdlib.h>

/*
 * Recursion depth is bounded by the parser's §6.3 limits (statement
 * nesting and expression height), so these recursive frees cannot
 * overflow the stack.
 */

void expr_free(Expr *expression) {
    if (expression == NULL) {
        return;
    }

    switch (expression->kind) {
        case EXPR_VARIABLE:
        case EXPR_LITERAL:
            break;

        case EXPR_UNARY:
            expr_free(expression->as.unary.operand);
            break;

        case EXPR_BINARY:
            expr_free(expression->as.binary.left);
            expr_free(expression->as.binary.right);
            break;

        case EXPR_ASSIGNMENT:
            expr_free(expression->as.assignment.value);
            break;

        case EXPR_CALL:
            for (size_t i = 0; i < expression->as.call.argument_count; ++i) {
                expr_free(expression->as.call.arguments[i]);
            }

            free(expression->as.call.arguments);
            break;
    }

    free(expression);
}

static void stmt_list_free(Stmt **statements, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        stmt_free(statements[i]);
    }

    free(statements);
}

void stmt_free(Stmt *statement) {
    if (statement == NULL) {
        return;
    }

    switch (statement->kind) {
        case STMT_EXPRESSION:
            expr_free(statement->as.expression);
            break;

        case STMT_EMPTY:
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;

        case STMT_BLOCK:
            stmt_list_free(
                statement->as.block.statements,
                statement->as.block.statement_count
            );
            break;

        case STMT_IF:
            expr_free(statement->as.if_statement.condition);
            stmt_free(statement->as.if_statement.then_branch);
            stmt_free(statement->as.if_statement.else_branch);
            break;

        case STMT_WHILE:
            expr_free(statement->as.while_statement.condition);
            stmt_free(statement->as.while_statement.body);
            break;

        case STMT_DO:
            stmt_free(statement->as.do_statement.body);
            expr_free(statement->as.do_statement.condition);
            break;
    }

    free(statement);
}

void program_free(Program *program) {
    stmt_list_free(program->statements, program->statement_count);
    program->statements = NULL;
    program->statement_count = 0;
}
