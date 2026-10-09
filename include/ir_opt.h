#ifndef LIL_IR_OPT_H
#define LIL_IR_OPT_H

#include "diagnostic.h"
#include "ir.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * IR optimization (-O1, docs/spec.md §8.7). The hook between lowering
 * (or ir_parse()) and every consumer of IR: main.c calls it for
 * --dump-ir and --interp, after ir_verify() succeeded, and verifies the
 * result again.
 *
 * Every pass is conservative. The IR is not SSA and vregs cross blocks
 * (§8.1), so value facts (known constants, copies, the value of a global)
 * are tracked only along a chain of blocks where each next block is
 * entered only from the one before, and are killed on every
 * redefinition; the one other cross-block fact is "this vreg has exactly
 * one definition in the whole program and it is 'const C'", which §8.4
 * rule 4 makes true at every reachable use. Every pass keeps the program
 * valid for ir_verify() and preserves the §5.7 observable behaviour (putc
 * bytes, exit status, SIGFPE) that the interpreter defines; no pass ever
 * makes an execution take more steps (§8.6).
 */

/* The first five are the "value passes": they only visit reachable
 * blocks and run together in one sweep when adjacent in a pass list. */
typedef enum {
    IR_PASS_LOADFWD,        /* store/load -> copy forwarding */
    IR_PASS_COPYPROP,       /* copy propagation */
    IR_PASS_FOLD,           /* constant folding (via ir_eval_*) */
    IR_PASS_SIMPLIFY,       /* algebraic identities: x+0, x*1, x*0, ... */
    IR_PASS_BRANCH,         /* br on a known constant (or L, L) -> jmp */
    IR_PASS_DCE,            /* dead pure instructions, self copies */
    IR_PASS_THREAD,         /* jumps to blocks that only 'jmp L' */
    IR_PASS_UNREACHABLE,    /* blocks the entry cannot reach */
    IR_PASS_MERGE           /* a block into its only predecessor's jmp */
} Ir_Pass;

#define IR_PASS_COUNT ((int)IR_PASS_MERGE + 1)

/* The driver stops after this many rounds even when a step still
 * changes something. -O1 never gets near it (every change decreases a
 * well-founded measure, and a chain of constant ifs folds in one sweep,
 * §8.7); a --passes list in an unhelpful order can. */
#define IR_OPT_MAX_ROUNDS 1000

/* The pass's name ("fold", "simplify", ...), as --passes spells it. */
const char *ir_pass_name(Ir_Pass pass);
/* The pass called 'name' (length bytes); false when there is none. */
bool ir_pass_from_name(const char *name, size_t length, Ir_Pass *pass);

/*
 * Runs one pass: its body is repeated until it changes nothing, so
 * running the pass again changes nothing (the repetitions always end,
 * §8.7; in practice there are one or two). *changed tells whether the
 * program changed.
 * Precondition: ir_verify(program). Returns false only when out of
 * memory (reported in *error); the program is then still valid but may
 * be partly rewritten.
 */
bool ir_run_pass(Ir_Program *program, Ir_Pass pass, bool *changed,
                 Diagnostic *error);

/*
 * Renumbers vregs densely (0..n-1), keeping their order, and sets
 * vreg_count to n. Returns false only when out of memory.
 */
bool ir_compact_vregs(Ir_Program *program, Diagnostic *error);

typedef struct {
    size_t rounds;          /* rounds run (the last one changed nothing) */
    bool reached_fixpoint;  /* false only when IR_OPT_MAX_ROUNDS was hit */
} Ir_Opt_Stats;

/*
 * Runs 'passes' (count of them, in order) in rounds until a round changes
 * nothing, at most IR_OPT_MAX_ROUNDS rounds. Each step of a round is
 * repeated until it changes nothing; a step is one pass, or a run of
 * adjacent value passes, which sweep the program together. Unless NDEBUG
 * is defined, ir_verify() runs after every step that changed the
 * program, and a failure is reported as an internal error ("IR
 * verification failed after pass 'NAME': ...", NAME like "fold" or
 * "fold+branch") -- a compiler bug. 'stats' may be NULL.
 * Returns false on out of memory or such a failure. Hitting
 * IR_OPT_MAX_ROUNDS is not a failure -- the program is valid and behaves
 * the same, it is only not fully optimized -- but *error is then set to
 * an internal warning ("the optimizer stopped after 1000 rounds without
 * reaching a fixpoint") and stats->reached_fixpoint is false.
 */
bool ir_optimize_passes(Ir_Program *program, const Ir_Pass *passes,
                        size_t count, Ir_Opt_Stats *stats, Diagnostic *error);

/* ir_optimize_passes() with a round limit other than IR_OPT_MAX_ROUNDS
 * (for tests of the limit). */
bool ir_optimize_passes_bounded(Ir_Program *program, const Ir_Pass *passes,
                                size_t count, size_t max_rounds,
                                Ir_Opt_Stats *stats, Diagnostic *error);

/*
 * Level 0 leaves the program unchanged. Level 1 compacts the vregs, runs
 * every pass (in Ir_Pass order) with ir_optimize_passes(), and compacts
 * the vregs again. Returns false as ir_optimize_passes() does, and
 * passes on its warning.
 */
bool ir_optimize(Ir_Program *program, int level, Diagnostic *error);

#endif
