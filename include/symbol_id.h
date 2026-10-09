#ifndef LIL_SYMBOL_ID_H
#define LIL_SYMBOL_ID_H

#include <stdint.h>

/*
 * "No symbol": the id of an unresolved name. Shared by the AST (an
 * Expr.symbol the parser creates, before semantic analysis resolves it)
 * and the symbol table (a failed lookup). Valid ids are 0..count-1, so
 * this can never be mistaken for one.
 */
#define SYMBOL_NONE SIZE_MAX

#endif
