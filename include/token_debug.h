#ifndef LIL_TOKEN_DEBUG_H
#define LIL_TOKEN_DEBUG_H

#include "diagnostic.h"
#include "source.h"
#include "token.h"

#include <stdbool.h>
#include <stdio.h>

/* Prints one token in the --dump-tokens format (spec §6.4.1):
 * "<Kind> <line>:<column> <spelling>", ending with a newline. */
void token_debug_print(FILE *stream, const Source *source, Token token);

/*
 * Lexes the whole source and prints every token, including the final End
 * token. On a lexical error the tokens before it are printed, the error
 * is stored in *diagnostic and false is returned.
 */
bool token_debug_dump(
    FILE *stream,
    const Source *source,
    Diagnostic *diagnostic
);

#endif
