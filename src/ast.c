#include "ast.h"

#include <stdlib.h>

void expr_free(Expr *expression) {
    if (expression == NULL) {
        return;
    }

    if (expression->kind == EXPR_BINARY) {
        expr_free(expression->as.binary.left);
        expr_free(expression->as.binary.right);
    } else if (expression->kind == EXPR_ASSIGNMENT) {
        expr_free(expression->as.assignment.value);
    }

    free(expression);
}

void program_free(Program *program) {
    for (size_t i = 0; i < program->expression_count; ++i) {
        expr_free(program->expressions[i]);
    }

    free(program->expressions);
    program->expressions = NULL;
    program->expression_count = 0;
}
