#include "operator.h"

#include <stddef.h>

/* docs/spec.md §4.2, binary operators from loosest to tightest. */
static const Binary_Operator binary_operators[] = {
    {TOKEN_ASSIGN, OPERATOR_PRECEDENCE_ASSIGNMENT, OPERATOR_RIGHT},
    {TOKEN_LOGICAL_OR, 2, OPERATOR_LEFT},
    {TOKEN_LOGICAL_AND, 3, OPERATOR_LEFT},
    {TOKEN_PIPE, 4, OPERATOR_LEFT},
    {TOKEN_AMPERSAND, 5, OPERATOR_LEFT},
    {TOKEN_EQUAL_EQUAL, 6, OPERATOR_LEFT},
    {TOKEN_NOT_EQUAL, 6, OPERATOR_LEFT},
    {TOKEN_LESS, 7, OPERATOR_LEFT},
    {TOKEN_LESS_EQUAL, 7, OPERATOR_LEFT},
    {TOKEN_GREATER, 7, OPERATOR_LEFT},
    {TOKEN_GREATER_EQUAL, 7, OPERATOR_LEFT},
    {TOKEN_PLUS, 8, OPERATOR_LEFT},
    {TOKEN_MINUS, 8, OPERATOR_LEFT},
    {TOKEN_STAR, 9, OPERATOR_LEFT},
    {TOKEN_SLASH, 9, OPERATOR_LEFT},
};

const Binary_Operator *operator_binary(Token_Kind kind) {
    for (size_t i = 0;
         i < sizeof(binary_operators) / sizeof(binary_operators[0]);
         ++i) {
        if (binary_operators[i].kind == kind) {
            return &binary_operators[i];
        }
    }

    return NULL;
}

bool operator_is_unary(Token_Kind kind) {
    return kind == TOKEN_MINUS || kind == TOKEN_TILDE;
}
