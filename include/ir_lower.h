#ifndef LIL_IR_LOWER_H
#define LIL_IR_LOWER_H

#include "ast.h"
#include "ir.h"
#include "symbol.h"

#include <stdbool.h>

/*
 * AST -> IR lowering (docs/spec.md §8.5).
 *
 * Precondition: 'program' and 'symbols' passed semantic analysis, so
 * sema_verify() holds (include/sema.h); lowering does no user-error
 * checking. 'out' receives one global per symbol (same ids and names)
 * and the blocks of the program, which end by loading r0 (or the
 * constant 0 when the program never mentions r0) and exiting with it.
 *
 * Evaluation order follows §4.3-§4.4 (left operand first, an assignment's
 * value before its store); '&&' and '||' become branches (§4.9), so the
 * short-circuit rule lives only here. The result satisfies ir_verify().
 *
 * Returns false only when out of memory; 'out' is then left empty.
 */
bool ir_lower_program(
    const Program *program,
    const Symbol_Table *symbols,
    Ir_Program *out
);

#endif
