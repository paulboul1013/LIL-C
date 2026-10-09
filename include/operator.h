#ifndef LIL_OPERATOR_H
#define LIL_OPERATOR_H

#include "token.h"

#include <stdbool.h>

/*
 * The single operator table of docs/spec.md §4.2. The Pratt parser and
 * the pretty-printer both read it, so adding an operator (or changing a
 * precedence) only means editing the table in src/operator.c.
 *
 * Larger numbers bind tighter. Binary operators use 1..9; prefix unary
 * operators and primaries (names, literals, calls, parentheses) bind
 * tighter than every binary operator.
 */

typedef enum {
    OPERATOR_LEFT,
    OPERATOR_RIGHT
} Operator_Associativity;

typedef struct {
    Token_Kind kind;
    int precedence;
    Operator_Associativity associativity;
} Binary_Operator;

#define OPERATOR_PRECEDENCE_ASSIGNMENT 1
#define OPERATOR_PRECEDENCE_UNARY 10
#define OPERATOR_PRECEDENCE_PRIMARY 11

/* The table entry for a binary operator token, or NULL. */
const Binary_Operator *operator_binary(Token_Kind kind);

/* Whether 'kind' is a prefix unary operator ('-' or '~'). */
bool operator_is_unary(Token_Kind kind);

#endif
