#ifndef LIL_AST_DEBUG_H
#define LIL_AST_DEBUG_H

#include "ast.h"
#include "source.h"

#include <stdio.h>

void ast_debug_print_program(
    FILE *stream,
    const Source *source,
    const Program *program
);

#endif
