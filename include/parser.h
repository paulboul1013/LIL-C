#ifndef LIL_PARSER_H
#define LIL_PARSER_H

#include "ast.h"
#include "lexer.h"

#include <stdbool.h>
typedef struct Parser Parser;

Parser *parser_create(Lexer *lexer);
void parser_destroy(Parser *parser);

bool parser_parse_program(Parser *parser, Program *out);
const Diagnostic *parser_diagnostic(const Parser *parser);

#endif
