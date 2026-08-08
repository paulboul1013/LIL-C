#ifndef LIL_PARSER_H
#define LIL_PARSER_H

#include "lexer.h"
#include "token.h"

#include <stdbool.h>
#include <stddef.h>

//one operation:
//+y,=x,*z
//operator_token = + 
//operand        = y
typedef struct {
    Token operator_token;
    Token operand;
} Flat_Operation;

// r0 = x + y * z;
// first = r0
// operation:
/*
    [0] = x
    [1] + Y 
    [2] * z
*/
typedef struct {
    Token first;
    
    Flat_Operation *operations;
    
    size_t operation_count;
    size_t operation_capacity;
} Flat_Expression;


typedef struct {
    Lexer *lexer;

    Token current;

    bool had_error;
    char error_message[160];
} Parser;

bool parser_init(Parser *parser,Lexer *lexer);

bool parser_parse_flat_expression(Parser *parser,Flat_Expression *expression);

const char *parser_error_message(const Parser *parser);

void flat_expression_free(Flat_Expression *expression);

void flat_expression_print_ascii(const Flat_Expression *expression);

#endif

