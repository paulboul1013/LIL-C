#include "ir_opt.h"

#include "ir_interp.h"
#include "ir_verify.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * See include/ir_opt.h and docs/spec.md §8.7.
 *
 * Termination (§8.7). Each step (one pass, or adjacent value passes
 * sweeping together), whenever it changes the program, strictly
 * decreases the measure
 *
 *   ( instructions,  weight,  resolvable copy uses,  threadable targets )
 *
 * compared lexicographically, where 'weight' sums
 *   3 for a binary or compare opcode, 2 for neg/not/load, 1 for copy and
 *   br, 0 for everything else,
 * a 'resolvable copy use' is an operand that copyprop would rewrite, and
 * a 'threadable target' is a jump target that thread would rewrite:
 *
 *   loadfwd     load -> copy                         weight -2+1
 *   copyprop    operand rewrites, nothing else       resolvable uses -> 0
 *   fold        X -> const (X is never a const)      weight decreases
 *   simplify    binary -> const, copy or neg         weight decreases
 *   branch      br -> jmp                            weight -1
 *   dce         deletes instructions                 instructions
 *   thread      retargets jumps only                 threadable -> 0
 *   unreachable deletes non-empty blocks             instructions
 *   merge       deletes a jmp per merged block       instructions
 *
 * and no pass increases an earlier component. A sweep in which only
 * copyprop changed something resolves every resolvable use: its facts
 * depend only on definitions and on the CFG, which copyprop leaves
 * alone, and it records each copy fact with the copy's operand already
 * resolved. All components are natural numbers, so the repetitions of a
 * step and the driver's rounds always end; IR_OPT_MAX_ROUNDS is only a
 * safety net. No pass adds an instruction to any execution path, so the
 * optimized program never needs more interpreter steps (§8.6).
 */

static const char *const pass_names[IR_PASS_COUNT] = {
    [IR_PASS_LOADFWD] = "loadfwd",
    [IR_PASS_COPYPROP] = "copyprop",
    [IR_PASS_FOLD] = "fold",
    [IR_PASS_SIMPLIFY] = "simplify",
    [IR_PASS_BRANCH] = "branch",
    [IR_PASS_DCE] = "dce",
    [IR_PASS_THREAD] = "thread",
    [IR_PASS_UNREACHABLE] = "unreachable",
    [IR_PASS_MERGE] = "merge",
};

const char *ir_pass_name(Ir_Pass pass) {
    return (int)pass >= 0 && (int)pass < IR_PASS_COUNT ? pass_names[pass]
                                                       : "?";
}

bool ir_pass_from_name(const char *name, size_t length, Ir_Pass *pass) {
    for (int i = 0; i < IR_PASS_COUNT; ++i) {
        if (strlen(pass_names[i]) == length &&
            memcmp(pass_names[i], name, length) == 0) {
            *pass = (Ir_Pass)i;
            return true;
        }
    }

    return false;
}

/* ---- helpers ------------------------------------------------------------ */

/* A zeroed array of count items (never NULL for count == 0 unless out of
 * memory). calloc() rejects count * size overflow. */
static void *zalloc(size_t count, size_t size) {
    return calloc(count == 0 ? 1 : count, size);
}

static bool out_of_memory(Diagnostic *error) {
    Source_Span span;

    memset(&span, 0, sizeof(span));
    diagnostic_set(error, DIAGNOSTIC_INTERNAL, span, "out of memory");
    return false;
}

static bool is_binary(Ir_Opcode opcode) {
    return ir_opcode_shape(opcode) == IR_SHAPE_BINARY;
}

static Ir_Instruction *terminator(Ir_Block *block) {
    return (Ir_Instruction *)ir_block_terminator(block);
}

/* A block that is just 'jmp L'. */
static bool is_forwarding(const Ir_Block *block) {
    return block->count == 1 && block->instructions[0].opcode == IR_JMP;
}

/*
 * Removes every block i with keep[i] == 0 (never blocks[0]) and renumbers
 * the rest in order. Every jump target of a kept block must be kept.
 */
static bool remove_blocks(Ir_Program *program, const unsigned char *keep) {
    const size_t count = program->block_count;
    size_t *map = zalloc(count, sizeof(size_t));

    if (map == NULL) {
        return false;
    }

    size_t kept = 0;

    for (size_t i = 0; i < count; ++i) {
        Ir_Block *block = &program->blocks[i];

        if (keep[i]) {
            map[i] = kept;
            program->blocks[kept++] = *block;
        } else {
            free(block->name);
            free(block->instructions);
        }
    }

    program->block_count = kept;

    for (size_t i = 0; i < kept; ++i) {
        Ir_Instruction *last = terminator(&program->blocks[i]);

        for (size_t t = 0; t < ir_opcode_target_count(last->opcode); ++t) {
            last->targets[t] = map[last->targets[t]];
        }
    }

    free(map);
    return true;
}

/* ---- known constants ---------------------------------------------------- */

/*
 * Known vreg values while walking one block (dce) or one chain of blocks
 * (the value passes): 'local' facts come from 'const' instructions
 * earlier in the block or chain and die when the vreg is redefined; 'global' facts hold for a vreg whose only definition in the
 * whole program is 'const C' (every reachable use then sees C, §8.4).
 */
typedef struct {
    int64_t *local_value;
    uint32_t *local_stamp;      /* == stamp: local_value is valid */
    uint32_t stamp;
    int64_t *global_value;
    unsigned char *definitions; /* 0, 1, or 2 meaning "more than one" */
    unsigned char *global_known;
} Consts;

static void consts_free(Consts *consts) {
    free(consts->local_value);
    free(consts->local_stamp);
    free(consts->global_value);
    free(consts->definitions);
    free(consts->global_known);
}

static bool consts_init(Consts *consts, const Ir_Program *program) {
    const size_t count = program->vreg_count;

    consts->local_value = zalloc(count, sizeof(int64_t));
    consts->local_stamp = zalloc(count, sizeof(uint32_t));
    consts->stamp = 0;
    consts->global_value = zalloc(count, sizeof(int64_t));
    consts->definitions = zalloc(count, 1);
    consts->global_known = zalloc(count, 1);

    if (consts->local_value == NULL || consts->local_stamp == NULL ||
        consts->global_value == NULL || consts->definitions == NULL ||
        consts->global_known == NULL) {
        consts_free(consts);
        return false;
    }

    for (size_t b = 0; b < program->block_count; ++b) {
        const Ir_Block *block = &program->blocks[b];

        for (size_t k = 0; k < block->count; ++k) {
            const Ir_Instruction *instruction = &block->instructions[k];

            if (!ir_opcode_has_dest(instruction->opcode)) {
                continue;
            }

            const size_t dest = instruction->dest;

            if (consts->definitions[dest] < 2) {
                ++consts->definitions[dest];
            }

            consts->global_known[dest] = instruction->opcode == IR_CONST;
            consts->global_value[dest] = instruction->value;
        }
    }

    for (size_t v = 0; v < count; ++v) {
        if (consts->definitions[v] != 1) {
            consts->global_known[v] = 0;
        }
    }

    return true;
}

static void consts_begin_block(Consts *consts, size_t vreg_count) {
    if (++consts->stamp == 0) {
        memset(consts->local_stamp, 0, vreg_count * sizeof(uint32_t));
        consts->stamp = 1;
    }
}

static bool consts_get(const Consts *consts, size_t vreg, int64_t *value) {
    if (consts->local_stamp[vreg] == consts->stamp) {
        *value = consts->local_value[vreg];
        return true;
    }

    if (consts->global_known[vreg]) {
        *value = consts->global_value[vreg];
        return true;
    }

    return false;
}

/* Records the definition made by 'instruction' (after any rewrite). */
static void consts_define(Consts *consts, const Ir_Instruction *instruction) {
    if (!ir_opcode_has_dest(instruction->opcode)) {
        return;
    }

    const size_t dest = instruction->dest;

    if (instruction->opcode != IR_CONST) {
        consts->local_stamp[dest] = 0;
        return;
    }

    consts->local_value[dest] = instruction->value;
    consts->local_stamp[dest] = consts->stamp;

    /* The only definition of dest is (now) this const. */
    if (consts->definitions[dest] == 1) {
        consts->global_known[dest] = 1;
        consts->global_value[dest] = instruction->value;
    }
}

/* Replaces 'instruction' by '%dest = const value', keeping its span. */
static void make_const(Ir_Instruction *instruction, int64_t value) {
    *instruction = ir_make_const(instruction->dest, value, instruction->span);
}

static void make_unary(Ir_Instruction *instruction, Ir_Opcode opcode,
                       size_t a) {
    *instruction =
        ir_make_unary(opcode, instruction->dest, a, instruction->span);
}

/* ---- simplify ----------------------------------------------------------- */

static bool is_commutative(Ir_Opcode opcode) {
    return opcode == IR_ADD || opcode == IR_MUL || opcode == IR_AND ||
           opcode == IR_OR || opcode == IR_EQ || opcode == IR_NE;
}

/*
 * One algebraic identity for 'instruction' (a binary opcode with at most
 * one known operand); returns whether it was rewritten. Never touches a
 * div except x / 1: x / x and 0 / x trap when x == 0, x / -1 traps for
 * INT64_MIN (§4.6).
 */
static bool simplify_one(Ir_Instruction *instruction, bool known_a,
                         int64_t va, bool known_b, int64_t vb) {
    const Ir_Opcode opcode = instruction->opcode;
    size_t a = instruction->operands[0];
    size_t b = instruction->operands[1];

    if (opcode == IR_SUB) {
        if (known_b && vb == 0) {
            make_unary(instruction, IR_COPY, a);         /* x - 0 */
        } else if (known_a && va == 0) {
            make_unary(instruction, IR_NEG, b);          /* 0 - x */
        } else if (a == b) {
            make_const(instruction, 0);                  /* x - x */
        } else {
            return false;
        }

        return true;
    }

    if (opcode == IR_DIV) {
        if (known_b && vb == 1) {
            make_unary(instruction, IR_COPY, a);         /* x / 1 */
            return true;
        }

        return false;
    }

    if (a == b) {
        switch (opcode) {
            case IR_AND:
            case IR_OR:
                make_unary(instruction, IR_COPY, a);     /* x & x, x | x */
                return true;
            case IR_EQ:
            case IR_LE:
            case IR_GE:
                make_const(instruction, 1);              /* x == x, ... */
                return true;
            case IR_NE:
            case IR_LT:
            case IR_GT:
                make_const(instruction, 0);              /* x != x, ... */
                return true;
            default:
                break;
        }
    }

    if (!is_commutative(opcode)) {
        return false;
    }

    /* Put the known operand (if any) on the right: C OP x -> x OP C. */
    if (known_a) {
        a = instruction->operands[1];
        known_b = true;
        vb = va;
    }

    if (!known_b) {
        return false;
    }

    switch (opcode) {
        case IR_ADD:
            if (vb == 0) {
                make_unary(instruction, IR_COPY, a);     /* x + 0 */
                return true;
            }
            break;
        case IR_MUL:
            if (vb == 0) {
                make_const(instruction, 0);              /* x * 0 */
                return true;
            }
            if (vb == 1) {
                make_unary(instruction, IR_COPY, a);     /* x * 1 */
                return true;
            }
            if (vb == -1) {
                make_unary(instruction, IR_NEG, a);      /* x * -1 */
                return true;
            }
            break;
        case IR_AND:
            if (vb == 0) {
                make_const(instruction, 0);              /* x & 0 */
                return true;
            }
            if (vb == -1) {
                make_unary(instruction, IR_COPY, a);     /* x & -1 */
                return true;
            }
            break;
        case IR_OR:
            if (vb == 0) {
                make_unary(instruction, IR_COPY, a);     /* x | 0 */
                return true;
            }
            if (vb == -1) {
                make_const(instruction, -1);             /* x | -1 */
                return true;
            }
            break;
        default:
            break;
    }

    return false;
}

/* ---- the walk shared by the value passes -------------------------------- */

/*
 * loadfwd, copyprop, fold, simplify and branch (the "value passes") share
 * one walk over the program, and any subset of them runs together in one
 * sweep: the -O1 driver runs all five at once, ir_run_pass() one.
 *
 * Only blocks reachable from the entry are visited. Unreachable blocks
 * never run and the unreachable pass deletes them, so the value passes
 * leave them exactly as they are; this also keeps a sweep linear (a value
 * pass never waits for facts from code that cannot run).
 *
 * Facts (known constants, copies, the vreg holding a global) are kept
 * along a chain of blocks: after block B, when B's terminator targets a
 * block S that is not the entry, not visited yet, not dead, and whose
 * only remaining incoming edge is that one, S is visited next and the
 * facts at the end of B stay valid at the start of S -- every execution
 * of S comes straight from the end of B. Otherwise the facts are dropped
 * and the walk goes on with the next unvisited block in reverse
 * postorder, so every block comes after the blocks dominating it (they
 * come before it in reverse postorder, and a chained S is dominated by
 * whatever dominates B).
 *
 * Incoming edges are counted over the blocks that may still run: at the
 * start the reachable ones. When branch turns a 'br' into a 'jmp', the
 * edge it drops is uncounted; a block left with no incoming edge (never
 * the entry) is dead -- it is not visited, and its own edges are
 * uncounted in turn. Edges only disappear during a sweep, so a count
 * never grows again: a block chained with count 1 really is entered only
 * from B. This is what lets one sweep follow a ladder of constant ifs to
 * its end: branch kills each else block as it goes, so the join after it
 * is chained to the then block and the next condition is known.
 */

#define WALK_PENDING 0
#define WALK_VISITED 1
#define WALK_DEAD 2

typedef struct {
    Ir_Program *program;
    size_t *rpo;            /* the reachable blocks in reverse postorder */
    size_t reachable;
    size_t next;            /* next rpo[] index to consider */
    size_t *incoming;       /* per block: counted incoming edges */
    unsigned char *state;   /* per block: WALK_* */
    size_t *stack;
    size_t current;         /* the block being visited, or IR_NONE */
} Walk;

static void walk_free(Walk *walk) {
    free(walk->rpo);
    free(walk->incoming);
    free(walk->state);
    free(walk->stack);
}

static bool walk_init(Walk *walk, Ir_Program *program) {
    const size_t count = program->block_count;
    size_t *next = zalloc(count, sizeof(size_t));

    walk->program = program;
    walk->rpo = zalloc(count, sizeof(size_t));
    walk->incoming = zalloc(count, sizeof(size_t));
    walk->state = zalloc(count, 1);
    walk->stack = zalloc(count, sizeof(size_t));
    walk->next = 0;
    walk->current = IR_NONE;

    if (next == NULL || walk->rpo == NULL || walk->incoming == NULL ||
        walk->state == NULL || walk->stack == NULL) {
        free(next);
        walk_free(walk);
        return false;
    }

    /* Iterative DFS; WALK_DEAD means "not seen yet", postorder is
     * written from the back of rpo[]. */
    size_t depth = 0;
    size_t position = count;

    memset(walk->state, WALK_DEAD, count);
    walk->stack[depth++] = 0;
    walk->state[0] = WALK_PENDING;

    while (depth > 0) {
        const size_t block = walk->stack[depth - 1];
        const Ir_Instruction *last =
            ir_block_terminator(&program->blocks[block]);

        if (next[block] < ir_opcode_target_count(last->opcode)) {
            const size_t target = last->targets[next[block]++];

            if (walk->state[target] == WALK_DEAD) {
                walk->state[target] = WALK_PENDING;
                walk->stack[depth++] = target;
            }
        } else {
            walk->rpo[--position] = block;
            --depth;
        }
    }

    walk->reachable = count - position;
    memmove(walk->rpo, walk->rpo + position,
            walk->reachable * sizeof(size_t));

    for (size_t i = 0; i < walk->reachable; ++i) {
        const Ir_Instruction *last =
            ir_block_terminator(&program->blocks[walk->rpo[i]]);

        for (size_t t = 0; t < ir_opcode_target_count(last->opcode); ++t) {
            ++walk->incoming[last->targets[t]];
        }
    }

    free(next);
    return true;
}

/*
 * The next block to visit; *chained tells whether the facts at the end
 * of the previous block hold at its start. False when every live block
 * has been visited.
 */
static bool walk_next(Walk *walk, size_t *block, bool *chained) {
    const size_t current = walk->current;

    if (current != IR_NONE && walk->state[current] == WALK_VISITED) {
        const Ir_Instruction *last =
            ir_block_terminator(&walk->program->blocks[current]);

        for (size_t t = 0; t < ir_opcode_target_count(last->opcode); ++t) {
            const size_t target = last->targets[t];

            if (target != 0 && walk->state[target] == WALK_PENDING &&
                walk->incoming[target] == 1) {
                walk->state[target] = WALK_VISITED;
                walk->current = target;
                *block = target;
                *chained = true;
                return true;
            }
        }
    }

    while (walk->next < walk->reachable) {
        const size_t candidate = walk->rpo[walk->next++];

        if (walk->state[candidate] == WALK_PENDING) {
            walk->state[candidate] = WALK_VISITED;
            walk->current = candidate;
            *block = candidate;
            *chained = false;
            return true;
        }
    }

    return false;
}

/* The visited block's terminator no longer jumps to 'target' (once). */
static void walk_drop_edge(Walk *walk, size_t target) {
    size_t depth = 0;

    if (--walk->incoming[target] != 0 || target == 0 ||
        walk->state[target] == WALK_DEAD) {
        return;
    }

    walk->state[target] = WALK_DEAD;
    walk->stack[depth++] = target;

    while (depth > 0) {
        const Ir_Instruction *last = ir_block_terminator(
            &walk->program->blocks[walk->stack[--depth]]);

        for (size_t t = 0; t < ir_opcode_target_count(last->opcode); ++t) {
            const size_t next = last->targets[t];

            if (--walk->incoming[next] == 0 && next != 0 &&
                walk->state[next] != WALK_DEAD) {
                walk->state[next] = WALK_DEAD;
                walk->stack[depth++] = next;
            }
        }
    }
}

/* ---- the value passes ---------------------------------------------------- */

#define VALUE_PASS(pass) (1u << (unsigned)(pass))

static bool is_value_pass(Ir_Pass pass) {
    return pass == IR_PASS_LOADFWD || pass == IR_PASS_COPYPROP ||
           pass == IR_PASS_FOLD || pass == IR_PASS_SIMPLIFY ||
           pass == IR_PASS_BRANCH;
}

/*
 * The facts of one chain. Constants are in 'consts'. The other facts
 * have the form "X currently holds the value of vreg R"; such a fact is
 * valid while R keeps the definition it had when the fact was recorded:
 * every definition gets a fresh number in version[] (a program-wide
 * counter, so it never needs resetting), and a fact stores the version
 * of R it saw and the chain it belongs to.
 */
typedef struct {
    unsigned passes;            /* VALUE_PASS() bits of the passes to run */
    Consts consts;
    uint64_t chain;             /* the current chain, from 1 */
    uint64_t *version;          /* per vreg: number of its latest definition */
    uint64_t counter;
    /* copyprop, per vreg d: d = copy source[d]. */
    size_t *source;
    uint64_t *source_version;
    uint64_t *source_chain;
    /* loadfwd, per global g: holder[g] holds the value of g. */
    size_t *holder;
    uint64_t *holder_version;
    uint64_t *holder_chain;
} Sweep;

static void sweep_free(Sweep *sweep) {
    consts_free(&sweep->consts);
    free(sweep->version);
    free(sweep->source);
    free(sweep->source_version);
    free(sweep->source_chain);
    free(sweep->holder);
    free(sweep->holder_version);
    free(sweep->holder_chain);
}

static bool sweep_init(Sweep *sweep, const Ir_Program *program,
                       unsigned passes) {
    const size_t vregs = program->vreg_count;
    const size_t globals = program->global_count;

    memset(sweep, 0, sizeof(*sweep));
    sweep->passes = passes;

    if (!consts_init(&sweep->consts, program)) {
        return false;
    }

    sweep->version = zalloc(vregs, sizeof(uint64_t));
    sweep->source = zalloc(vregs, sizeof(size_t));
    sweep->source_version = zalloc(vregs, sizeof(uint64_t));
    sweep->source_chain = zalloc(vregs, sizeof(uint64_t));
    sweep->holder = zalloc(globals, sizeof(size_t));
    sweep->holder_version = zalloc(globals, sizeof(uint64_t));
    sweep->holder_chain = zalloc(globals, sizeof(uint64_t));

    if (sweep->version == NULL || sweep->source == NULL ||
        sweep->source_version == NULL || sweep->source_chain == NULL ||
        sweep->holder == NULL || sweep->holder_version == NULL ||
        sweep->holder_chain == NULL) {
        sweep_free(sweep);
        return false;
    }

    return true;
}

static bool runs(const Sweep *sweep, Ir_Pass pass) {
    return (sweep->passes & VALUE_PASS(pass)) != 0;
}

/*
 * loadfwd: after 'store @g, %a' (or a load of @g into %a) earlier in the
 * chain, '%d = load @g' becomes '%d = copy %a' while %a keeps that
 * definition. Globals are only read and written by load and store
 * (§8.1), so nothing else can change @g in between.
 */
static bool forward_load(const Sweep *sweep, Ir_Instruction *instruction) {
    if (instruction->opcode != IR_LOAD) {
        return false;
    }

    const size_t global = instruction->global;
    const size_t holder = sweep->holder[global];

    if (sweep->holder_chain[global] != sweep->chain ||
        sweep->version[holder] != sweep->holder_version[global]) {
        return false;
    }

    make_unary(instruction, IR_COPY, holder);
    return true;
}

/*
 * copyprop: after '%d = copy %r' earlier in the chain, uses of %d read
 * %r instead, as long as neither %d nor %r has been redefined. The copy
 * itself stays (other blocks may use %d); dce removes it when nothing
 * does.
 */
static bool propagate_copies(const Sweep *sweep, Ir_Instruction *instruction) {
    bool changed = false;

    for (size_t i = 0; i < ir_opcode_operand_count(instruction->opcode); ++i) {
        const size_t use = instruction->operands[i];
        const size_t source = sweep->source[use];

        if (sweep->source_chain[use] == sweep->chain &&
            sweep->version[source] == sweep->source_version[use]) {
            instruction->operands[i] = source;
            changed = true;
        }
    }

    return changed;
}

/*
 * fold: an instruction whose operands all have known values becomes
 * 'const' of ir_eval_unary()/ir_eval_binary() -- the interpreter's own
 * arithmetic, so wraparound agrees (§5.7). A div that would trap
 * (ir_eval_binary() returns false) is left exactly as it is.
 */
static bool fold(const Consts *consts, Ir_Instruction *instruction) {
    const Ir_Opcode opcode = instruction->opcode;
    int64_t a = 0;
    int64_t b = 0;
    int64_t result = 0;

    if (ir_opcode_shape(opcode) == IR_SHAPE_UNARY &&
        consts_get(consts, instruction->operands[0], &a)) {
        make_const(instruction, ir_eval_unary(opcode, a));
        return true;
    }

    if (is_binary(opcode) &&
        consts_get(consts, instruction->operands[0], &a) &&
        consts_get(consts, instruction->operands[1], &b) &&
        ir_eval_binary(opcode, a, b, &result)) {
        make_const(instruction, result);
        return true;
    }

    return false;
}

/* simplify: one algebraic identity (simplify_one()) for a binary
 * instruction with at most one known operand. */
static bool simplify(const Consts *consts, Ir_Instruction *instruction) {
    if (!is_binary(instruction->opcode)) {
        return false;
    }

    int64_t va = 0;
    int64_t vb = 0;
    const bool known_a = consts_get(consts, instruction->operands[0], &va);
    const bool known_b = consts_get(consts, instruction->operands[1], &vb);

    /* Both known is fold's job (and a trapping div stays). */
    return !(known_a && known_b) &&
           simplify_one(instruction, known_a, va, known_b, vb);
}

/* branch: 'br %c, L1, L2' with a known %c, or with L1 == L2, becomes a
 * jmp; the walk forgets the edge it drops. */
static bool fold_branch(const Consts *consts, Walk *walk,
                        Ir_Instruction *instruction) {
    int64_t condition = 0;
    size_t kept = 0;

    if (instruction->opcode != IR_BR) {
        return false;
    }

    if (instruction->targets[0] == instruction->targets[1]) {
        kept = 0;
    } else if (consts_get(consts, instruction->operands[0], &condition)) {
        kept = condition != 0 ? 0 : 1;
    } else {
        return false;
    }

    const size_t dropped = instruction->targets[1 - kept];

    *instruction = ir_make_jmp(instruction->targets[kept], instruction->span);
    walk_drop_edge(walk, dropped);
    return true;
}

/*
 * fold turned 'before' into 'const value': an operand of 'before' that
 * holds 'value' right now (the source of a copy, x in x * 1 or x + 0, ...),
 * or IR_NONE. The new const then also works as a copy of that operand:
 * copyprop sends later uses to the operand and dce deletes the const, so
 * folding does not leave a second vreg holding the same constant.
 */
static size_t folded_copy(const Consts *consts, const Ir_Instruction *before,
                          int64_t value) {
    for (size_t i = 0; i < ir_opcode_operand_count(before->opcode); ++i) {
        int64_t known = 0;

        if (consts_get(consts, before->operands[i], &known) &&
            known == value) {
            return before->operands[i];
        }
    }

    return IR_NONE;
}

/*
 * Records the facts the (rewritten) instruction establishes. 'copied' is
 * a vreg that (rewritten or not) the instruction copies, or IR_NONE; see
 * folded_copy().
 */
static void sweep_define(Sweep *sweep, const Ir_Instruction *instruction,
                         bool forwarded, size_t copied) {
    consts_define(&sweep->consts, instruction);

    if (instruction->opcode == IR_COPY) {
        copied = instruction->operands[0];
    }

    if (ir_opcode_has_dest(instruction->opcode)) {
        const size_t dest = instruction->dest;

        sweep->version[dest] = ++sweep->counter;
        sweep->source_chain[dest] = 0;

        if (copied != IR_NONE && copied != dest) {
            sweep->source[dest] = copied;
            sweep->source_version[dest] =
                sweep->version[sweep->source[dest]];
            sweep->source_chain[dest] = sweep->chain;
        }
    }

    /* A load forwarded to a copy leaves the fact on the old holder, so a
     * second sweep sees exactly the same facts. */
    if (!forwarded && (instruction->opcode == IR_LOAD ||
                       instruction->opcode == IR_STORE)) {
        const size_t global = instruction->global;

        sweep->holder[global] = instruction->opcode == IR_LOAD
                                    ? instruction->dest
                                    : instruction->operands[0];
        sweep->holder_version[global] =
            sweep->version[sweep->holder[global]];
        sweep->holder_chain[global] = sweep->chain;
    }
}

/* One sweep of the value passes in 'passes' (VALUE_PASS() bits). */
static bool sweep_values(Ir_Program *program, unsigned passes,
                         bool *changed) {
    Walk walk;
    Sweep sweep;
    size_t index = 0;
    bool chained = false;

    if (!walk_init(&walk, program)) {
        return false;
    }

    if (!sweep_init(&sweep, program, passes)) {
        walk_free(&walk);
        return false;
    }

    while (walk_next(&walk, &index, &chained)) {
        Ir_Block *block = &program->blocks[index];

        if (!chained) {
            ++sweep.chain;
            consts_begin_block(&sweep.consts, program->vreg_count);
        }

        for (size_t k = 0; k < block->count; ++k) {
            Ir_Instruction *instruction = &block->instructions[k];
            bool forwarded = false;

            if (runs(&sweep, IR_PASS_LOADFWD) &&
                forward_load(&sweep, instruction)) {
                forwarded = true;
                *changed = true;
            }

            if (runs(&sweep, IR_PASS_COPYPROP) &&
                propagate_copies(&sweep, instruction)) {
                *changed = true;
            }

            const Ir_Instruction before = *instruction;
            size_t copied = IR_NONE;

            if (runs(&sweep, IR_PASS_FOLD) &&
                fold(&sweep.consts, instruction)) {
                copied = folded_copy(&sweep.consts, &before,
                                     instruction->value);
                *changed = true;
            } else if ((runs(&sweep, IR_PASS_SIMPLIFY) &&
                        simplify(&sweep.consts, instruction)) ||
                       (runs(&sweep, IR_PASS_BRANCH) &&
                        fold_branch(&sweep.consts, &walk, instruction))) {
                *changed = true;
            }

            sweep_define(&sweep, instruction, forwarded, copied);
        }
    }

    sweep_free(&sweep);
    walk_free(&walk);
    return true;
}

/* ---- dce ---------------------------------------------------------------- */

/*
 * Whether the instruction must stay even when its result is unused: it
 * has an effect (store, putc, terminators) or it is a div that may trap
 * (§5.7). A div is known not to trap when its divisor is a known constant
 * other than 0 and -1, or is -1 with a known dividend other than
 * INT64_MIN.
 */
static bool is_root(const Ir_Instruction *instruction, const Consts *consts) {
    if (!ir_opcode_has_dest(instruction->opcode)) {
        return true;
    }

    if (instruction->opcode != IR_DIV) {
        return false;
    }

    int64_t divisor = 0;
    int64_t dividend = 0;

    if (!consts_get(consts, instruction->operands[1], &divisor) ||
        divisor == 0) {
        return true;
    }

    return divisor == -1 &&
           (!consts_get(consts, instruction->operands[0], &dividend) ||
            dividend == INT64_MIN);
}

static bool is_self_copy(const Ir_Instruction *instruction) {
    return instruction->opcode == IR_COPY &&
           instruction->operands[0] == instruction->dest;
}

/*
 * Dead code elimination. A vreg is live when a root uses it, or when an
 * instruction defining a live vreg uses it (the IR is not SSA: every
 * definition of a live vreg is kept). Uses in every block count,
 * unreachable ones too. Non-root instructions defining a dead vreg are
 * deleted, and so is every '%d = copy %d' (it changes nothing).
 */
static bool pass_dce(Ir_Program *program, bool *changed) {
    const size_t vregs = program->vreg_count;
    size_t total = 0;

    for (size_t b = 0; b < program->block_count; ++b) {
        total += program->blocks[b].count;
    }

    Consts consts;
    /* root[] is per instruction, numbered in program order. */
    unsigned char *root = zalloc(total, 1);
    unsigned char *live = zalloc(vregs, 1);
    /* Definitions of each vreg, as program-order instruction numbers:
     * def_list[def_start[v] .. def_start[v + 1]). */
    size_t *def_start = zalloc(vregs + 1, sizeof(size_t));
    size_t *def_list = zalloc(total, sizeof(size_t));
    const Ir_Instruction **by_number = zalloc(total, sizeof(*by_number));
    size_t *worklist = zalloc(vregs, sizeof(size_t));
    size_t *fill = zalloc(vregs, sizeof(size_t));
    bool ok = root != NULL && live != NULL && def_start != NULL &&
              def_list != NULL && by_number != NULL && worklist != NULL &&
              fill != NULL;

    if (ok && !consts_init(&consts, program)) {
        ok = false;
    }

    if (!ok) {
        free(root);
        free(live);
        free(def_start);
        free(def_list);
        free(by_number);
        free(worklist);
        free(fill);
        return false;
    }

    size_t number = 0;
    size_t pending = 0;

    for (size_t b = 0; b < program->block_count; ++b) {
        const Ir_Block *block = &program->blocks[b];

        consts_begin_block(&consts, vregs);

        for (size_t k = 0; k < block->count; ++k, ++number) {
            const Ir_Instruction *instruction = &block->instructions[k];

            by_number[number] = instruction;
            root[number] = is_root(instruction, &consts);

            if (ir_opcode_has_dest(instruction->opcode)) {
                ++def_start[instruction->dest + 1];
            }

            if (root[number]) {
                for (size_t i = 0;
                     i < ir_opcode_operand_count(instruction->opcode); ++i) {
                    const size_t use = instruction->operands[i];

                    if (!live[use]) {
                        live[use] = 1;
                        worklist[pending++] = use;
                    }
                }
            }

            consts_define(&consts, instruction);
        }
    }

    for (size_t v = 0; v < vregs; ++v) {
        def_start[v + 1] += def_start[v];
    }

    /* def_list, grouped by vreg (fill[v] is v's next free slot). */
    for (size_t v = 0; v < vregs; ++v) {
        fill[v] = def_start[v];
    }

    for (size_t n = 0; n < total; ++n) {
        if (ir_opcode_has_dest(by_number[n]->opcode)) {
            def_list[fill[by_number[n]->dest]++] = n;
        }
    }

    while (pending > 0) {
        const size_t vreg = worklist[--pending];

        for (size_t d = def_start[vreg]; d < def_start[vreg + 1]; ++d) {
            const Ir_Instruction *definition = by_number[def_list[d]];

            for (size_t i = 0;
                 i < ir_opcode_operand_count(definition->opcode); ++i) {
                const size_t use = definition->operands[i];

                if (!live[use]) {
                    live[use] = 1;
                    worklist[pending++] = use;
                }
            }
        }
    }

    number = 0;

    for (size_t b = 0; b < program->block_count; ++b) {
        Ir_Block *block = &program->blocks[b];
        size_t kept = 0;

        for (size_t k = 0; k < block->count; ++k, ++number) {
            const Ir_Instruction *instruction = &block->instructions[k];
            const bool dead = is_self_copy(instruction) ||
                              (!root[number] && !live[instruction->dest]);

            if (dead) {
                *changed = true;
            } else {
                block->instructions[kept++] = *instruction;
            }
        }

        block->count = kept;
    }

    consts_free(&consts);
    free(root);
    free(live);
    free(def_start);
    free(def_list);
    free(by_number);
    free(worklist);
    free(fill);
    return true;
}

/* ---- thread ------------------------------------------------------------- */

/*
 * Jump threading: a jump (jmp or either br target) to a block that is just
 * 'jmp L' goes to the end of that chain of forwarding blocks instead. A
 * chain that ends in a cycle of forwarding blocks (for example
 * 'a: jmp a') is left alone. final[] is computed once per block with an
 * explicit stack, so long chains are linear and cycles cannot hang.
 */
static bool pass_thread(Ir_Program *program, bool *changed) {
    const size_t count = program->block_count;
    size_t *final = zalloc(count, sizeof(size_t));
    size_t *stack = zalloc(count, sizeof(size_t));
    unsigned char *state = zalloc(count, 1);  /* 0 new, 1 on stack, 2 done */

    if (final == NULL || stack == NULL || state == NULL) {
        free(final);
        free(stack);
        free(state);
        return false;
    }

    for (size_t start = 0; start < count; ++start) {
        size_t depth = 0;
        size_t block = start;

        while (is_forwarding(&program->blocks[block]) && state[block] == 0) {
            state[block] = 1;
            stack[depth++] = block;
            block = program->blocks[block].instructions[0].targets[0];
        }

        /* Where jumps to the stacked blocks should go (IR_NONE: stay). */
        size_t result = block;

        if (is_forwarding(&program->blocks[block])) {
            result = state[block] == 1 ? IR_NONE : final[block];
        }

        while (depth > 0) {
            const size_t done = stack[--depth];

            final[done] = result;
            state[done] = 2;
        }
    }

    for (size_t b = 0; b < count; ++b) {
        Ir_Instruction *last = terminator(&program->blocks[b]);

        for (size_t t = 0; t < ir_opcode_target_count(last->opcode); ++t) {
            const size_t target = last->targets[t];

            if (is_forwarding(&program->blocks[target]) &&
                final[target] != IR_NONE) {
                last->targets[t] = final[target];
                *changed = true;
            }
        }
    }

    free(final);
    free(stack);
    free(state);
    return true;
}

/* ---- merge -------------------------------------------------------------- */

/*
 * Block merging: when block B ends with 'jmp S', S is not the entry, not
 * B, and B's jmp is S's only incoming edge (counting every block, also
 * unreachable ones), S's instructions replace B's jmp and S is deleted.
 * B keeps absorbing while its new terminator qualifies.
 */
static bool pass_merge(Ir_Program *program, bool *changed) {
    const size_t count = program->block_count;
    size_t *predecessors = zalloc(count, sizeof(size_t));
    unsigned char *keep = zalloc(count, 1);
    bool ok = predecessors != NULL && keep != NULL;
    bool merged = false;

    for (size_t b = 0; ok && b < count; ++b) {
        const Ir_Instruction *last = terminator(&program->blocks[b]);

        keep[b] = 1;

        for (size_t t = 0; t < ir_opcode_target_count(last->opcode); ++t) {
            ++predecessors[last->targets[t]];
        }
    }

    for (size_t b = 0; ok && b < count; ++b) {
        Ir_Block *block = &program->blocks[b];

        while (keep[b]) {
            const Ir_Instruction *last = terminator(block);
            const size_t target = last->targets[0];

            if (last->opcode != IR_JMP || target == b || target == 0 ||
                predecessors[target] != 1 || !keep[target]) {
                break;
            }

            const Ir_Block *absorbed = &program->blocks[target];
            /* Grow first, so running out of memory changes nothing. */
            const size_t needed = block->count - 1 + absorbed->count;

            if (needed > block->capacity) {
                /* Doubling keeps a long chain of merges linear. */
                size_t capacity = block->capacity * 2;

                if (capacity < needed) {
                    capacity = needed;
                }

                Ir_Instruction *grown =
                    capacity > SIZE_MAX / sizeof(Ir_Instruction)
                        ? NULL
                        : realloc(block->instructions,
                                  capacity * sizeof(Ir_Instruction));

                if (grown == NULL) {
                    ok = false;
                    break;
                }

                block->instructions = grown;
                block->capacity = capacity;
            }

            /* Replace the jmp by the absorbed block's instructions. */
            memcpy(&block->instructions[block->count - 1],
                   absorbed->instructions,
                   absorbed->count * sizeof(Ir_Instruction));
            block->count = needed;
            keep[target] = 0;
            merged = true;
        }
    }

    if (ok && merged) {
        ok = remove_blocks(program, keep);
        *changed = true;
    }

    free(predecessors);
    free(keep);
    return ok;
}

/* ---- unreachable -------------------------------------------------------- */

/* Deletes every block the entry cannot reach; the rest keep their order
 * (so blocks[0] stays the entry) and their unique names. */
static bool pass_unreachable(Ir_Program *program, bool *changed) {
    const size_t count = program->block_count;
    unsigned char *seen = zalloc(count, 1);
    size_t *stack = zalloc(count, sizeof(size_t));
    bool ok = seen != NULL && stack != NULL;

    if (ok) {
        size_t depth = 0;
        size_t reached = 1;

        seen[0] = 1;
        stack[depth++] = 0;

        while (depth > 0) {
            const Ir_Instruction *last =
                ir_block_terminator(&program->blocks[stack[--depth]]);

            for (size_t t = 0; t < ir_opcode_target_count(last->opcode); ++t) {
                const size_t target = last->targets[t];

                if (!seen[target]) {
                    seen[target] = 1;
                    stack[depth++] = target;
                    ++reached;
                }
            }
        }

        if (reached < count) {
            ok = remove_blocks(program, seen);
            *changed = true;
        }
    }

    free(seen);
    free(stack);
    return ok;
}

/* ---- driver ------------------------------------------------------------- */

typedef bool (*Pass_Function)(Ir_Program *program, bool *changed);

/* The passes that are not value passes (those run in sweep_values()). */
static const Pass_Function pass_functions[IR_PASS_COUNT] = {
    [IR_PASS_DCE] = pass_dce,
    [IR_PASS_THREAD] = pass_thread,
    [IR_PASS_UNREACHABLE] = pass_unreachable,
    [IR_PASS_MERGE] = pass_merge,
};

/*
 * Runs one step until it changes nothing: a sweep of the value passes
 * 'values' (VALUE_PASS() bits) when there are any, otherwise 'pass'.
 * So a step is idempotent by construction (running it twice equals
 * running it once). Every repetition that changes something decreases
 * the measure at the top of this file, so the loop ends; in practice a
 * step needs one or two repetitions (see §8.7). Returns false only when
 * out of memory.
 */
static bool run_step(Ir_Program *program, unsigned values, Ir_Pass pass,
                     bool *changed) {
    *changed = false;

    for (;;) {
        bool body_changed = false;
        const bool ok = values != 0
                            ? sweep_values(program, values, &body_changed)
                            : pass_functions[pass](program, &body_changed);

        if (!ok) {
            return false;
        }

        if (!body_changed) {
            return true;
        }

        *changed = true;
    }
}

bool ir_run_pass(Ir_Program *program, Ir_Pass pass, bool *changed,
                 Diagnostic *error) {
    diagnostic_clear(error);

    if (!run_step(program, is_value_pass(pass) ? VALUE_PASS(pass) : 0, pass,
                  changed)) {
        return out_of_memory(error);
    }

    return true;
}

bool ir_compact_vregs(Ir_Program *program, Diagnostic *error) {
    const size_t count = program->vreg_count;
    size_t *renumber = zalloc(count, sizeof(size_t));

    diagnostic_clear(error);

    if (renumber == NULL) {
        return out_of_memory(error);
    }

    /* renumber[v] = 1 when used, then the new number. */
    for (size_t b = 0; b < program->block_count; ++b) {
        const Ir_Block *block = &program->blocks[b];

        for (size_t k = 0; k < block->count; ++k) {
            const Ir_Instruction *instruction = &block->instructions[k];

            if (ir_opcode_has_dest(instruction->opcode)) {
                renumber[instruction->dest] = 1;
            }

            for (size_t i = 0; i < ir_opcode_operand_count(instruction->opcode);
                 ++i) {
                renumber[instruction->operands[i]] = 1;
            }
        }
    }

    size_t used = 0;

    for (size_t v = 0; v < count; ++v) {
        const bool is_used = renumber[v] != 0;

        renumber[v] = used;
        used += is_used;
    }

    for (size_t b = 0; b < program->block_count; ++b) {
        Ir_Block *block = &program->blocks[b];

        for (size_t k = 0; k < block->count; ++k) {
            Ir_Instruction *instruction = &block->instructions[k];

            if (ir_opcode_has_dest(instruction->opcode)) {
                instruction->dest = renumber[instruction->dest];
            }

            for (size_t i = 0; i < ir_opcode_operand_count(instruction->opcode);
                 ++i) {
                instruction->operands[i] = renumber[instruction->operands[i]];
            }
        }
    }

    program->vreg_count = used;
    free(renumber);
    return true;
}

#ifndef NDEBUG
/* Debug and test builds: every pass must leave a valid program (§8.4).
 * 'name' names the step (one pass, or value passes run together). */
static bool verify_after(const Ir_Program *program, const char *name,
                         Diagnostic *error) {
    Diagnostic problem;

    if (ir_verify(program, &problem)) {
        return true;
    }

    diagnostic_set(error, DIAGNOSTIC_INTERNAL, problem.span,
                   "IR verification failed after pass '%s': %s", name,
                   problem.message);
    return false;
}

/* "fold" or, for value passes run together, "fold+branch". */
static void step_name(const Ir_Pass *passes, size_t count, char *name,
                      size_t size) {
    size_t used = 0;

    name[0] = '\0';

    for (size_t i = 0; i < count && used < size; ++i) {
        const int written = snprintf(name + used, size - used, "%s%s",
                                     i == 0 ? "" : "+",
                                     ir_pass_name(passes[i]));

        used += written > 0 ? (size_t)written : 0;
    }
}
#endif

bool ir_optimize_passes(Ir_Program *program, const Ir_Pass *passes,
                        size_t count, Ir_Opt_Stats *stats, Diagnostic *error) {
    return ir_optimize_passes_bounded(program, passes, count,
                                      IR_OPT_MAX_ROUNDS, stats, error);
}

bool ir_optimize_passes_bounded(Ir_Program *program, const Ir_Pass *passes,
                                size_t count, size_t max_rounds,
                                Ir_Opt_Stats *stats, Diagnostic *error) {
    Ir_Opt_Stats local;
    bool changed = true;

    if (stats == NULL) {
        stats = &local;
    }

    stats->rounds = 0;
    stats->reached_fixpoint = false;
    diagnostic_clear(error);

    while (changed && stats->rounds < max_rounds) {
        changed = false;
        ++stats->rounds;

        for (size_t i = 0; i < count;) {
            /* Consecutive value passes run together as one sweep. */
            unsigned values = 0;
            size_t end = i;
            bool step_changed = false;

            while (end < count && is_value_pass(passes[end])) {
                values |= VALUE_PASS(passes[end]);
                ++end;
            }

            if (end == i) {
                ++end;
            }

            if (!run_step(program, values, passes[i], &step_changed)) {
                return out_of_memory(error);
            }

#ifndef NDEBUG
            if (step_changed) {
                char name[128];

                step_name(passes + i, end - i, name, sizeof(name));

                if (!verify_after(program, name, error)) {
                    return false;
                }
            }
#endif

            changed = changed || step_changed;
            i = end;
        }
    }

    stats->reached_fixpoint = !changed;

    if (changed) {
        /* Never seen (§8.7): the program is still correct, only not
         * optimized as far as it could be. */
        Source_Span span;

        memset(&span, 0, sizeof(span));
        diagnostic_set_warning(error, DIAGNOSTIC_INTERNAL, span,
                               "the optimizer stopped after %zu rounds "
                               "without reaching a fixpoint",
                               max_rounds);
    }

    return true;
}

bool ir_optimize(Ir_Program *program, int level, Diagnostic *error) {
    static const Ir_Pass all_passes[IR_PASS_COUNT] = {
        IR_PASS_LOADFWD, IR_PASS_COPYPROP, IR_PASS_FOLD, IR_PASS_SIMPLIFY,
        IR_PASS_BRANCH, IR_PASS_DCE, IR_PASS_THREAD, IR_PASS_UNREACHABLE,
        IR_PASS_MERGE,
    };
    Diagnostic warning;

    diagnostic_clear(error);

    if (level < 1) {
        return true;
    }

    if (!ir_compact_vregs(program, error) ||
        !ir_optimize_passes(program, all_passes, IR_PASS_COUNT, NULL,
                            error)) {
        return false;
    }

    /* Keep a fixpoint warning across the second compaction. */
    warning = *error;

    if (!ir_compact_vregs(program, error)) {
        return false;
    }

    *error = warning;
    return true;
}
