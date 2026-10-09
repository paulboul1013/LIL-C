#ifndef LIL_TOKEN_H
#define LIL_TOKEN_H

#include "source.h"

#include <stdint.h>

typedef enum {
    TOKEN_END,
    TOKEN_INVALID,

    //basic value
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_CHARACTER,

    //keyword
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_DO,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    
    //
    TOKEN_LEFT_PAREN,   //(
    TOKEN_RIGHT_PAREN,  //)
    TOKEN_LEFT_BRACE,   //{
    TOKEN_RIGHT_BRACE,  //}
    TOKEN_SEMICOLON,    //;
    TOKEN_COMMA,        //,

    //operator
    TOKEN_ASSIGN,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,

    //bit and logic operator
    TOKEN_AMPERSAND,
    TOKEN_PIPE,
    TOKEN_TILDE,
    TOKEN_LOGICAL_AND,
    TOKEN_LOGICAL_OR,

    //compare operator
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,
    TOKEN_GREATER,
    TOKEN_GREATER_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_NOT_EQUAL,

} Token_Kind;

typedef struct {
    Token_Kind kind;
    Source_Span span;
    /* TOKEN_NUMBER: the literal's value (§2.7, always 0..INT64_MAX).
     * TOKEN_CHARACTER: the unsigned byte value (§2.6, 0..255).
     * 0 for every other kind. */
    int64_t value;
} Token;

const char* token_kind_name(Token_Kind kind);

#endif
