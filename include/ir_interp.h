#ifndef LIL_IR_INTERP_H
#define LIL_IR_INTERP_H

#include "ir.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The IR interpreter (docs/spec.md §8.6): the reference ("correct
 * answer") semantics that every later stage -- -O0, -O1, the x86-64
 * back end and the assembler -- must reproduce exactly (§5.7).
 *
 * Precondition: the program satisfies ir_verify().
 */

/* --interp runs at most this many instructions by default (§6.4). */
#define IR_INTERP_DEFAULT_STEP_LIMIT UINT64_C(1000000000)

typedef enum {
    IR_INTERP_EXITED,       /* reached 'exit' */
    IR_INTERP_TRAPPED,      /* a div trapped (§4.6): SIGFPE natively */
    IR_INTERP_STEP_LIMIT,   /* executed step_limit instructions first */
    IR_INTERP_OUT_OF_MEMORY
} Ir_Interp_Status;

typedef struct {
    Ir_Interp_Status status;
    /* IR_INTERP_EXITED: the exit operand and the status, value & 0xff. */
    int64_t exit_value;
    int exit_code;
    /* Instructions executed (including the one that trapped or exited). */
    uint64_t steps;
    /* IR_INTERP_TRAPPED: the span of the trapping div. */
    Source_Span trap_span;
} Ir_Interp_Result;

/*
 * Runs 'program' from its entry block with every global and vreg 0.
 * Each putc writes one byte to 'output' (write errors are ignored, as
 * natively, §4.10). Stops at 'exit', at a trap, or after 'step_limit'
 * instructions (0 = no limit).
 */
void ir_interp_run(
    const Ir_Program *program,
    FILE *output,
    uint64_t step_limit,
    Ir_Interp_Result *result
);

/*
 * Ends the process the way the native program would (§5.5, §5.6):
 * flushes stdout, then exits with result->exit_code, or for a trap
 * restores the default SIGFPE action, unblocks it and raises it, so the
 * status is death by SIGFPE (128 + 8 = 136 in the shell). Only for
 * IR_INTERP_EXITED and IR_INTERP_TRAPPED.
 */
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void ir_interp_terminate(const Ir_Interp_Result *result);

/* Part of ir_interp_terminate(): makes SIGFPE's action the default and
 * unblocks it, so raising it ends the process. */
void ir_interp_restore_sigfpe(void);

/*
 * §8.2 evaluation of one value-producing opcode on operand values; the
 * single definition of the IR's arithmetic, shared by the interpreter
 * and (part B) the constant folder so they cannot disagree. Returns false
 * exactly when the instruction traps (div by 0, INT64_MIN / -1); *result
 * is then unchanged. ir_eval_binary() takes binary and compare opcodes,
 * ir_eval_unary() copy, neg and not.
 */
bool ir_eval_binary(Ir_Opcode opcode, int64_t a, int64_t b, int64_t *result);
int64_t ir_eval_unary(Ir_Opcode opcode, int64_t a);

#endif
