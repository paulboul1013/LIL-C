#ifndef LIL_AST_PRINT_H
#define LIL_AST_PRINT_H

#include "ast.h"
#include "source.h"

#include <stddef.h>

/*
 * Pretty-prints a program back to LIL-C source. Names and literals keep
 * their original spelling; parentheses are inserted only where §4.2
 * precedence and associativity require them.
 *
 * Contract (round trip): for every program P produced by the parser,
 * parsing ast_print_program(P) succeeds and yields a program with the same
 * structure, operators, names and literals as P (only spans differ), and
 * printing that program again gives the identical text.
 *
 * Returns a malloc'd NUL-terminated string (its length in *out_length) or
 * NULL when out of memory.
 */
char *ast_print_program(
    const Source *source,
    const Program *program,
    size_t *out_length
);

#endif
