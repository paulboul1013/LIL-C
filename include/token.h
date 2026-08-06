#ifndef LIL_TOKEN_H
#define LIL_TOKEN_H

#include <stddef.h>

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

    //token in the source code of start position
    //not including '\0' in the end
    const char *start;

    //token character count
    size_t length;

    //debug info
    //line number
    size_t line;

    //column number
    size_t column;

} Token;

const char* token_kind_name(Token_Kind kind);

#endif