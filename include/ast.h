#ifndef LIL_AST_H
#define LIL_AST_H

#include "token.h"

#include <stddef.h>

typedef enum {
    EXPR_VARIABLE,
    EXPR_LITERAL,
    EXPR_BINARY,
    EXPR_ASSIGNMENT
} Expr_Kind;

typedef struct Expr Expr;

struct Expr {
    Expr_Kind kind;
    Source_Span span;
    union {
        Token variable;
        Token literal;
        struct {
            Expr *left;
            Token operator_token;
            Expr *right;
        } binary;
        struct {
            Token name;
            Token equals_token;
            Expr *value;
        } assignment;
    } as;
};

typedef struct {
    Expr **expressions;
    size_t expression_count;
} Program;

void expr_free(Expr *expression);
void program_free(Program *program);

#endif
