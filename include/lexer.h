#ifndef LIL_LEXER_H
#define LIL_LEXER_H

#include "diagnostic.h"
#include "token.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const Source *source;

    //pointer to next not read yet character
    size_t position;

    //next not read yet character position 
    //line and column from 1 index
    size_t line;
    size_t column;

    Diagnostic diagnostic;
} Lexer;

void lexer_init(Lexer *lexer, const Source *source);
Token lexer_next_token(Lexer *lexer);

const Diagnostic *lexer_diagnostic(const Lexer *lexer);


#endif
