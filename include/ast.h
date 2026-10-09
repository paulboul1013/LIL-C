#ifndef LIL_AST_H
#define LIL_AST_H

#include "symbol_id.h"
#include "token.h"

#include <stddef.h>

/*
 * AST for LIL-C v1 (docs/spec.md §3, §4). Every node carries the span of
 * the source text it was parsed from (§6.4.3). Parentheses do not create
 * nodes; a parenthesized expression's span includes the parentheses.
 */

typedef enum {
    EXPR_VARIABLE,
    EXPR_LITERAL,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_ASSIGNMENT,
    EXPR_CALL
} Expr_Kind;

/* §4.10 builtin functions; set on EXPR_CALL by semantic analysis. */
typedef enum {
    BUILTIN_NONE,       /* not resolved (the parser leaves every call so) */
    BUILTIN_PUTC
} Builtin_Kind;

typedef struct Expr Expr;

struct Expr {
    Expr_Kind kind;
    Source_Span span;
    /*
     * Filled in by semantic analysis (src/sema.c, see include/sema.h for
     * the invariants). The parser sets symbol to SYMBOL_NONE and builtin
     * to BUILTIN_NONE, so an unanalyzed tree never looks resolved.
     *   symbol:  EXPR_VARIABLE and EXPR_ASSIGNMENT: the variable's id in
     *            the Symbol_Table (0..count-1); SYMBOL_NONE elsewhere.
     *   builtin: EXPR_CALL: which builtin is called.
     */
    size_t symbol;
    Builtin_Kind builtin;
    union {
        Token variable;
        /* TOKEN_NUMBER or TOKEN_CHARACTER; the value is in literal.value. */
        Token literal;
        struct {
            Token operator_token;   /* TOKEN_MINUS or TOKEN_TILDE */
            Expr *operand;
        } unary;
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
        /* The parser accepts any name and any number of arguments;
         * semantic analysis checks both (§4.10). */
        struct {
            Token name;
            Expr **arguments;
            size_t argument_count;
        } call;
    } as;
};

typedef enum {
    STMT_EXPRESSION,
    STMT_EMPTY,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_DO,
    STMT_BREAK,
    STMT_CONTINUE
} Stmt_Kind;

typedef struct Stmt Stmt;

struct Stmt {
    Stmt_Kind kind;
    Source_Span span;
    union {
        Expr *expression;
        struct {
            Stmt **statements;
            size_t statement_count;
        } block;
        struct {
            Expr *condition;
            Stmt *then_branch;
            Stmt *else_branch;      /* NULL when there is no 'else' */
        } if_statement;
        struct {
            Expr *condition;
            Stmt *body;
        } while_statement;
        struct {
            Stmt *body;
            Expr *condition;
        } do_statement;
    } as;
};

typedef struct {
    Source_Span span;               /* the whole source file */
    Stmt **statements;
    size_t statement_count;
} Program;

/* Each *_free accepts NULL; program_free() may be called repeatedly. */
void expr_free(Expr *expression);
void stmt_free(Stmt *statement);
void program_free(Program *program);

#endif
