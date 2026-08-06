#ifndef LIL_LEXER_H
#define LIL_LEXER_H

#include "token.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *source;
    size_t source_length;

    //pointer to next not read yet character
    size_t position;

    //next not read yet character position 
    //line and column from 1 index
    size_t line;
    size_t column;

    bool had_error;
    char error_message[256];
} Lexer;

void lexer_init(Lexer *lexer,const char *source,size_t source_length);
Token lexer_next_token(Lexer *lexer);

const char *lexer_error_message(const Lexer *lexer);


#endif