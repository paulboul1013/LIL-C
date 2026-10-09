#ifndef LIL_PARSER_H
#define LIL_PARSER_H

#include "ast.h"
#include "lexer.h"

#include <stdbool.h>

/*
 * §6.3 limits. PARSER_MAX_NESTING bounds syntactic nesting ('=' chains,
 * parentheses, call argument lists, unary operators, blocks and control
 * statements); PARSER_MAX_EXPR_HEIGHT bounds the height of every
 * expression tree, including long left-associative operator chains.
 * Exceeding either is the syntax error "nesting too deep".
 */
#define PARSER_MAX_NESTING 256
#define PARSER_MAX_EXPR_HEIGHT 1024

typedef struct Parser Parser;

Parser *parser_create(Lexer *lexer);
void parser_destroy(Parser *parser);

/*
 * Parses the whole program. Syntax errors are recovered from (§6.3.4) and
 * collected, up to DIAGNOSTIC_MAX_ERRORS; a lexical error stops parsing.
 * Returns true and fills *out only when there were no errors; otherwise
 * *out is left empty (safe to program_free()).
 */
bool parser_parse_program(Parser *parser, Program *out);

/* The first error, or an empty diagnostic when there was none. */
const Diagnostic *parser_diagnostic(const Parser *parser);
/* Every reported error, in source order. */
const Diagnostic_List *parser_diagnostics(const Parser *parser);

#endif
