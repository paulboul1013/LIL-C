#ifndef LIL_IR_VERIFY_H
#define LIL_IR_VERIFY_H

#include "diagnostic.h"
#include "ir.h"

#include <stdbool.h>

/*
 * The IR verifier (docs/spec.md §8.4). Checks, in this order, and reports
 * the first violation in *error (stage DIAGNOSTIC_VERIFY, at the span of
 * the offending instruction or block):
 *
 *   1. the program has at least one block, and block names are unique;
 *   2. every opcode is valid, every vreg operand and destination the
 *      opcode needs is < vreg_count, every global id is < global_count,
 *      and every jump target is an existing block;
 *   3. every block ends with exactly one terminator (jmp, br, exit): its
 *      last instruction, and no other;
 *   4. definite definition: in every block reachable from the entry
 *      (blocks[0]), every vreg use is preceded by a definition of that
 *      vreg on every path from the entry to it. Nothing is defined when
 *      the entry starts. Unreachable blocks (orphans left by folding a
 *      constant 'br', cycles nothing reaches, ...) are never executed:
 *      rule 4 does not check them and they do not count as
 *      predecessors. The first violation in program order is reported.
 *
 * Rules 1-3 apply to every block. Consumers (the backend, later passes)
 * must skip or tolerate unreachable blocks, whose vregs may be undefined.
 *
 * Rule 4 is a forward "definitely defined" dataflow in reverse postorder
 * over densely renumbered vregs: near-linear in practice, with bitsets
 * bounded to 64 MiB in total. Besides that it allocates about
 * 8 bytes per vreg_count (sparse vreg numbers count by their maximum).
 *
 * Passes must keep every rule; tests run the verifier after every pass.
 * Returns true when the program is valid.
 */
bool ir_verify(const Ir_Program *program, Diagnostic *error);

#endif
