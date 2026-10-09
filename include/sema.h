#ifndef LIL_SEMA_H
#define LIL_SEMA_H

#include "ast.h"
#include "diagnostic.h"
#include "source.h"
#include "symbol.h"

#include <stdbool.h>

/*
 * Semantic analysis (docs/spec.md §3.7, §4.10, §5, §6.2).
 *
 * sema_analyze() walks a parsed Program once, in source order. It builds
 * the symbol table, resolves every variable reference to a symbol id,
 * marks builtin calls, and reports
 *   errors   (stage DIAGNOSTIC_SEMANTIC): 'break'/'continue' outside a
 *            loop, a call to anything but putc, putc with an argument
 *            count other than 1, putc read or assigned as a variable;
 *   warnings (§6.2): an expression statement without effect, a variable
 *            that is assigned but never read (never for r0, which the
 *            program's exit reads, §5.5).
 * Errors are capped at DIAGNOSTIC_MAX_ERRORS and are in strictly
 * increasing source order. Warnings are the first DIAGNOSTIC_MAX_WARNINGS
 * in source order, also strictly increasing.
 *
 * POST-SEMA INVARIANTS. When sema_analyze() returns true (no errors; there
 * may be warnings), the Program and Symbol_Table satisfy all of the
 * following, which later stages (IR lowering, §6.3.1) may rely on without
 * checking for user errors again. sema_verify() checks them.
 *
 *  1. Every EXPR_VARIABLE and EXPR_ASSIGNMENT has expr->symbol <
 *     symbols->count (never SYMBOL_NONE, the parser's default), and
 *     that symbol's name equals the node's spelling after ASCII case
 *     folding (§5.2). Equal names have equal ids.
 *  2. No symbol is named "putc": putc is never read or assigned.
 *  3. Every EXPR_CALL has builtin == BUILTIN_PUTC and exactly one
 *     argument. There are no other calls.
 *  4. Every STMT_BREAK and STMT_CONTINUE is nested (through any number of
 *     blocks and if branches) in the body of a STMT_WHILE or STMT_DO.
 *  5. Symbol ids are dense, 0..count-1, and name distinct symbols
 *     (symbol_table_find(name) == id), numbered in order of first
 *     appearance in the source (strictly increasing first_span); a symbol
 *     that is never read first appears at its first write. All variables
 *     are global and start at 0 (§5.1). The exit code is read from sema_exit_symbol() (§5.5), which
 *     is SYMBOL_NONE when the program never mentions r0 (exit code 0).
 *  6. Inherited from the parser: every child pointer that the grammar
 *     requires is non-NULL (only if_statement.else_branch may be NULL),
 *     variable names and assignment targets are TOKEN_IDENTIFIER,
 *     literal values are in 0..INT64_MAX (TOKEN_NUMBER) or 0..255
 *     (TOKEN_CHARACTER), unary operators are '-' or '~', binary
 *     operators are the §4.2 table entries other than '=' (which is an
 *     EXPR_ASSIGNMENT), and the tree height is bounded (§6.3.2, §6.3.3),
 *     so recursive passes are safe.
 *
 * Division is never checked here: division by zero is a run-time trap
 * (§4.6), not a compile-time error.
 */

/*
 * Analyzes 'program' (which it annotates in place) into 'symbols' (which
 * must be freshly initialized). Appends errors to 'errors' and warnings
 * to 'warnings' (both expected to be empty). Returns true when no error
 * was reported. Running out of memory is an internal error.
 */
bool sema_analyze(
    Program *program,
    const Source *source,
    Symbol_Table *symbols,
    Diagnostic_List *errors,
    Diagnostic_List *warnings
);

/* The id of r0 (any case), or SYMBOL_NONE (§5.5). */
size_t sema_exit_symbol(const Symbol_Table *symbols);

/*
 * Checks the post-sema invariants 1-6 above, except the tree-height bound
 * of 6 (the parser enforces it; a deeper tree would not reach here). Also
 * checks that every symbol is mentioned at least once. Lowering may then
 * treat any other node kind, operator or builtin as unreachable.
 */
bool sema_verify(
    const Program *program,
    const Source *source,
    const Symbol_Table *symbols
);

#endif
