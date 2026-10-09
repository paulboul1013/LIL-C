#include "ir_verify.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See include/ir_verify.h and docs/spec.md §8.4. */

/* Names longer than this are shortened in messages (like sema.c). */
#define VERIFY_MAX_NAME_IN_MESSAGE 200

static int shown_length(const char *name) {
    const size_t length = strlen(name);
    return length > VERIFY_MAX_NAME_IN_MESSAGE ? VERIFY_MAX_NAME_IN_MESSAGE
                                               : (int)length;
}

static const char *ellipsis(const char *name) {
    return strlen(name) > VERIFY_MAX_NAME_IN_MESSAGE ? "..." : "";
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static bool fail(Diagnostic *error, Source_Span span, const char *format,
                 ...) {
    char message[sizeof(error->message)];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    diagnostic_set(error, DIAGNOSTIC_VERIFY, span, "%s", message);
    return false;
}

static int compare_names(const void *left, const void *right) {
    const Ir_Block *const *a = left;
    const Ir_Block *const *b = right;
    return strcmp((*a)->name, (*b)->name);
}

/* Rule 1: block names are unique. */
static bool verify_unique_names(const Ir_Program *program,
                                Diagnostic *error) {
    const Ir_Block **sorted =
        malloc(program->block_count * sizeof(*sorted));

    if (sorted == NULL) {
        return fail(error, program->blocks[0].span,
                    "out of memory while verifying IR");
    }

    for (size_t i = 0; i < program->block_count; ++i) {
        sorted[i] = &program->blocks[i];
    }

    qsort(sorted, program->block_count, sizeof(*sorted), compare_names);

    for (size_t i = 1; i < program->block_count; ++i) {
        if (strcmp(sorted[i - 1]->name, sorted[i]->name) == 0) {
            const Ir_Block *second = sorted[i - 1] > sorted[i]
                ? sorted[i - 1] : sorted[i];
            const bool ok = fail(error, second->span,
                                 "duplicate block '%.*s%s'",
                                 shown_length(second->name), second->name,
                                 ellipsis(second->name));
            free(sorted);
            return ok;
        }
    }

    free(sorted);
    return true;
}

static bool verify_vreg(const Ir_Program *program,
                        const Ir_Instruction *instruction, size_t vreg,
                        Diagnostic *error) {
    if (vreg == IR_NONE) {
        return fail(error, instruction->span,
                    "'%s' is missing a virtual register",
                    ir_opcode_name(instruction->opcode));
    }

    if (vreg >= program->vreg_count) {
        return fail(error, instruction->span,
                    "virtual register %%%zu is out of range "
                    "(the program has %zu)",
                    vreg, program->vreg_count);
    }

    return true;
}

/* Rule 2 for one instruction. */
static bool verify_operands(const Ir_Program *program,
                            const Ir_Instruction *instruction,
                            Diagnostic *error) {
    const Ir_Opcode opcode = instruction->opcode;

    if ((int)opcode < 0 || (int)opcode >= IR_OPCODE_COUNT) {
        return fail(error, instruction->span, "invalid opcode %d",
                    (int)opcode);
    }

    if (ir_opcode_has_dest(opcode) &&
        !verify_vreg(program, instruction, instruction->dest, error)) {
        return false;
    }

    for (size_t i = 0; i < ir_opcode_operand_count(opcode); ++i) {
        if (!verify_vreg(program, instruction, instruction->operands[i],
                         error)) {
            return false;
        }
    }

    if ((opcode == IR_LOAD || opcode == IR_STORE) &&
        instruction->global >= program->global_count) {
        return fail(error, instruction->span,
                    "'%s' refers to a global that does not exist",
                    ir_opcode_name(opcode));
    }

    for (size_t i = 0; i < ir_opcode_target_count(opcode); ++i) {
        if (instruction->targets[i] >= program->block_count) {
            return fail(error, instruction->span,
                        "'%s' jumps to a block that does not exist",
                        ir_opcode_name(opcode));
        }
    }

    return true;
}

/* Rules 2 and 3. */
static bool verify_structure(const Ir_Program *program, Diagnostic *error) {
    for (size_t b = 0; b < program->block_count; ++b) {
        const Ir_Block *block = &program->blocks[b];

        for (size_t k = 0; k < block->count; ++k) {
            const Ir_Instruction *instruction = &block->instructions[k];

            if (!verify_operands(program, instruction, error)) {
                return false;
            }

            if (ir_opcode_is_terminator(instruction->opcode) &&
                k + 1 != block->count) {
                return fail(error, instruction->span,
                            "terminator '%s' is not the last instruction "
                            "of block '%.*s%s'",
                            ir_opcode_name(instruction->opcode),
                            shown_length(block->name), block->name,
                            ellipsis(block->name));
            }
        }

        if (ir_block_terminator(block) == NULL) {
            return fail(error, block->span,
                        "block '%.*s%s' does not end with a terminator",
                        shown_length(block->name), block->name,
                        ellipsis(block->name));
        }
    }

    return true;
}

/* ---- rule 4: definite definition -------------------------------------- */

/*
 * A forward "definitely defined" dataflow over the blocks reachable from
 * the entry:
 *
 *   IN[entry] = {}          IN[b] = intersection of OUT[p], p a reachable
 *                                   predecessor of b
 *   OUT[b] = IN[b] + the vregs b defines
 *
 * solved for the greatest fixpoint (OUT starts full) by a worklist kept in
 * reverse postorder. A use is valid when its vreg is in IN[b] or defined
 * earlier in b. Unreachable blocks are never executed and are not
 * checked.
 *
 * Only a few vregs need the dataflow. A use preceded by a definition in
 * its own block is checked locally; an upward-exposed use in the entry is
 * always an error (nothing is defined on entry); and a vreg defined
 * anywhere in the entry is defined in every other reachable block, since
 * every path starts by running the whole entry. The remaining vregs --
 * those with an upward-exposed use outside the entry and no definition in
 * it -- are "tracked" and numbered densely 0..tracked-1.
 *
 * Tracked vregs are first checked by a backward search per vreg
 * (search_uses()), which is linear when definitions are close to their
 * uses, as in lowered code. Only when that exceeds its budget does the
 * bitset dataflow run (solve_slices()): the bitsets do not depend on how
 * sparse the vreg numbers are, take at most VERIFY_BITSET_BUDGET words,
 * and with more tracked vregs than fit the dataflow runs once per slice
 * of them.
 */

/* Words (of 64 bits) of OUT bitsets alive at once: 64 MiB. */
#define VERIFY_BITSET_BUDGET ((size_t)1 << 23)

/* vreg_info[v]: v is not tracked, is defined in the entry, or is tracked
 * with dense id vreg_info[v] - VREG_TRACKED. */
#define VREG_UNTRACKED 0
#define VREG_ENTRY_DEFINED 1
#define VREG_TRACKED 2

typedef struct {
    size_t block;               /* reverse-postorder index of its block */
    size_t tracked;             /* dense id of the vreg */
    const Ir_Instruction *instruction;
    size_t vreg;
} Exposed_Use;

typedef struct {
    const Ir_Program *program;
    /* Reachable blocks in reverse postorder: rpo[0] is the entry;
     * order[b] is the position of block b, or IR_NONE when unreachable. */
    size_t reachable;
    size_t *rpo;
    size_t *order;
    /* Reachable predecessors (as positions) of the block at position i:
     * preds[pred_start[i] .. pred_start[i + 1]). */
    size_t *pred_start;
    size_t *preds;
    /* Successors (as positions) of the block at position i:
     * succs[succ_start[i] .. succ_start[i + 1]). */
    size_t *succ_start;
    size_t *succs;
    size_t *vreg_info;
    size_t tracked;
    /* Tracked vregs defined by the block at position i:
     * defs[def_start[i] .. def_start[i + 1]). */
    size_t *def_start;
    size_t *defs;
    /* Upward-exposed uses of tracked vregs, in program order. */
    Exposed_Use *uses;
    size_t use_count;
    size_t use_capacity;
} Def_Use;

static size_t target_position(const Def_Use *state,
                              const Ir_Instruction *terminator, size_t t) {
    return state->order[terminator->targets[t]];
}

/* Reverse postorder of the blocks reachable from the entry, by an
 * iterative depth-first search. */
static bool order_blocks(Def_Use *state) {
    const Ir_Program *program = state->program;
    const size_t count = program->block_count;
    size_t *stack = malloc(count * sizeof(size_t));
    size_t *next = calloc(count, sizeof(size_t));
    size_t *postorder = malloc(count * sizeof(size_t));
    size_t top = 0;
    size_t done = 0;

    state->order = malloc(count * sizeof(size_t));
    state->rpo = malloc(count * sizeof(size_t));

    if (stack == NULL || next == NULL || postorder == NULL ||
        state->order == NULL || state->rpo == NULL) {
        free(stack);
        free(next);
        free(postorder);
        return false;
    }

    for (size_t b = 0; b < count; ++b) {
        state->order[b] = IR_NONE;
    }

    /* order[b] == 0 marks "seen" during the search. */
    state->order[0] = 0;
    stack[top++] = 0;

    while (top > 0) {
        const size_t block = stack[top - 1];
        const Ir_Instruction *terminator =
            ir_block_terminator(&program->blocks[block]);

        if (next[block] < ir_opcode_target_count(terminator->opcode)) {
            const size_t target = terminator->targets[next[block]++];

            if (state->order[target] == IR_NONE) {
                state->order[target] = 0;
                stack[top++] = target;
            }
        } else {
            postorder[done++] = block;
            --top;
        }
    }

    state->reachable = done;

    for (size_t i = 0; i < done; ++i) {
        state->rpo[i] = postorder[done - 1 - i];
        state->order[state->rpo[i]] = i;
    }

    free(stack);
    free(next);
    free(postorder);
    return true;
}

static bool build_predecessors(Def_Use *state) {
    const Ir_Program *program = state->program;
    const size_t count = state->reachable;
    size_t edges = 0;

    state->pred_start = calloc(count + 1, sizeof(size_t));
    state->succ_start = calloc(count + 1, sizeof(size_t));

    if (state->pred_start == NULL || state->succ_start == NULL) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        const Ir_Instruction *terminator =
            ir_block_terminator(&program->blocks[state->rpo[i]]);
        const size_t targets = ir_opcode_target_count(terminator->opcode);

        for (size_t t = 0; t < targets; ++t) {
            ++state->pred_start[target_position(state, terminator, t) + 1];
        }

        edges += targets;
        state->succ_start[i + 1] = edges;
    }

    for (size_t i = 0; i < count; ++i) {
        state->pred_start[i + 1] += state->pred_start[i];
    }

    size_t *fill = malloc((count + 1) * sizeof(size_t));
    state->preds = malloc((edges + 1) * sizeof(size_t));
    state->succs = malloc((edges + 1) * sizeof(size_t));

    if (fill == NULL || state->preds == NULL || state->succs == NULL) {
        free(fill);
        return false;
    }

    memcpy(fill, state->pred_start, (count + 1) * sizeof(size_t));

    for (size_t i = 0; i < count; ++i) {
        const Ir_Instruction *terminator =
            ir_block_terminator(&program->blocks[state->rpo[i]]);

        for (size_t t = 0; t < ir_opcode_target_count(terminator->opcode);
             ++t) {
            const size_t successor = target_position(state, terminator, t);

            state->preds[fill[successor]++] = i;
            state->succs[state->succ_start[i] + t] = successor;
        }
    }

    free(fill);
    return true;
}

static bool add_use(Def_Use *state, const Exposed_Use *use) {
    if (state->use_count == state->use_capacity) {
        const size_t capacity =
            state->use_capacity == 0 ? 16 : state->use_capacity * 2;
        Exposed_Use *uses =
            realloc(state->uses, capacity * sizeof(*uses));

        if (uses == NULL) {
            return false;
        }

        state->uses = uses;
        state->use_capacity = capacity;
    }

    state->uses[state->use_count++] = *use;
    return true;
}

static void set_bit(uint64_t *bits, size_t index) {
    bits[index / 64] |= (uint64_t)1 << (index % 64);
}

static void clear_bit(uint64_t *bits, size_t index) {
    bits[index / 64] &= ~((uint64_t)1 << (index % 64));
}

static bool test_bit(const uint64_t *bits, size_t index) {
    return (bits[index / 64] >> (index % 64)) & 1;
}

/*
 * Scans every reachable block once with a "defined earlier in this block"
 * bitset. Reports an upward-exposed use in the entry; marks the vregs the
 * entry defines; numbers the tracked vregs and records their exposed
 * uses (in program order) and their definitions per block.
 */
static bool scan_blocks(Def_Use *state, uint64_t *local,
                        const Ir_Instruction **undefined_in_entry) {
    const Ir_Program *program = state->program;
    const Ir_Block *entry = &program->blocks[0];

    for (size_t k = 0; k < entry->count; ++k) {
        const Ir_Instruction *instruction = &entry->instructions[k];

        for (size_t i = 0; i < ir_opcode_operand_count(instruction->opcode);
             ++i) {
            if (!test_bit(local, instruction->operands[i])) {
                *undefined_in_entry = instruction;
                return true;
            }
        }

        if (ir_opcode_has_dest(instruction->opcode)) {
            set_bit(local, instruction->dest);
            state->vreg_info[instruction->dest] = VREG_ENTRY_DEFINED;
        }
    }

    for (size_t k = 0; k < entry->count; ++k) {
        if (ir_opcode_has_dest(entry->instructions[k].opcode)) {
            clear_bit(local, entry->instructions[k].dest);
        }
    }

    /* Exposed uses, block by block in program order. */
    for (size_t b = 1; b < program->block_count; ++b) {
        const Ir_Block *block = &program->blocks[b];

        if (state->order[b] == IR_NONE) {
            continue;
        }

        for (size_t k = 0; k < block->count; ++k) {
            const Ir_Instruction *instruction = &block->instructions[k];

            for (size_t i = 0;
                 i < ir_opcode_operand_count(instruction->opcode); ++i) {
                const size_t vreg = instruction->operands[i];
                size_t *info = &state->vreg_info[vreg];

                if (test_bit(local, vreg) || *info == VREG_ENTRY_DEFINED) {
                    continue;
                }

                if (*info == VREG_UNTRACKED) {
                    *info = VREG_TRACKED + state->tracked++;
                }

                const Exposed_Use use = {
                    state->order[b], *info - VREG_TRACKED, instruction, vreg
                };

                if (!add_use(state, &use)) {
                    return false;
                }
            }

            if (ir_opcode_has_dest(instruction->opcode)) {
                set_bit(local, instruction->dest);
            }
        }

        for (size_t k = 0; k < block->count; ++k) {
            if (ir_opcode_has_dest(block->instructions[k].opcode)) {
                clear_bit(local, block->instructions[k].dest);
            }
        }
    }

    /* Definitions of tracked vregs, per reachable block. */
    const size_t count = state->reachable;
    size_t total = 0;

    state->def_start = calloc(count + 1, sizeof(size_t));

    if (state->def_start == NULL) {
        return false;
    }

    for (size_t pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            state->defs = malloc((total + 1) * sizeof(size_t));

            if (state->defs == NULL) {
                return false;
            }

            total = 0;
        }

        for (size_t i = 0; i < count; ++i) {
            const Ir_Block *block = &program->blocks[state->rpo[i]];

            state->def_start[i] = total;

            for (size_t k = 0; k < block->count; ++k) {
                const Ir_Instruction *instruction = &block->instructions[k];

                if (ir_opcode_has_dest(instruction->opcode) &&
                    state->vreg_info[instruction->dest] >= VREG_TRACKED) {
                    if (pass == 1) {
                        state->defs[total] =
                            state->vreg_info[instruction->dest] -
                            VREG_TRACKED;
                    }
                    ++total;
                }
            }
        }

        state->def_start[count] = total;
    }

    return true;
}

/* in = intersection of OUT over the reachable predecessors of the block
 * at position i (the empty set for the entry). */
static void compute_in(const Def_Use *state, const uint64_t *out,
                       size_t words, size_t i, uint64_t *in) {
    if (i == 0) {
        memset(in, 0, words * sizeof(uint64_t));
        return;
    }

    const size_t first = state->preds[state->pred_start[i]];
    memcpy(in, &out[first * words], words * sizeof(uint64_t));

    for (size_t p = state->pred_start[i] + 1; p < state->pred_start[i + 1];
         ++p) {
        const uint64_t *pred = &out[state->preds[p] * words];

        for (size_t w = 0; w < words; ++w) {
            in[w] &= pred[w];
        }
    }
}

/* The next set bit at or after 'from' in 'bits' (n bits), or n. */
static size_t next_set_bit(const uint64_t *bits, size_t n, size_t from) {
    while (from < n) {
        const uint64_t word = bits[from / 64] >> (from % 64);

        if (word != 0) {
            size_t index = from;
            uint64_t rest = word;

            while ((rest & 1) == 0) {
                rest >>= 1;
                ++index;
            }

            return index < n ? index : n;
        }

        from = (from / 64 + 1) * 64;
    }

    return n;
}

/*
 * Solves the dataflow for tracked vregs [low, low + words * 64) and
 * lowers *first_bad to the first exposed use in that slice whose vreg is
 * not definitely defined.
 */
static void solve_slice(const Def_Use *state, size_t low, size_t words,
                        uint64_t *out, uint64_t *in, uint64_t *pending,
                        size_t *first_bad) {
    const size_t count = state->reachable;
    const size_t high = low + words * 64;

    memset(out, 0, words * sizeof(uint64_t));
    memset(&out[words], 0xff, (count - 1) * words * sizeof(uint64_t));
    memset(pending, 0, ((count + 63) / 64) * sizeof(uint64_t));

    for (size_t i = 1; i < count; ++i) {
        set_bit(pending, i);
    }

    size_t cursor = 1;

    for (;;) {
        size_t i = next_set_bit(pending, count, cursor);

        if (i == count) {
            i = next_set_bit(pending, count, 0);

            if (i == count) {
                break;
            }
        }

        clear_bit(pending, i);
        cursor = i + 1;
        compute_in(state, out, words, i, in);

        for (size_t d = state->def_start[i]; d < state->def_start[i + 1];
             ++d) {
            if (state->defs[d] >= low && state->defs[d] < high) {
                set_bit(in, state->defs[d] - low);
            }
        }

        if (memcmp(in, &out[i * words], words * sizeof(uint64_t)) == 0) {
            continue;
        }

        memcpy(&out[i * words], in, words * sizeof(uint64_t));

        for (size_t s = state->succ_start[i]; s < state->succ_start[i + 1];
             ++s) {
            if (state->succs[s] != 0) {
                set_bit(pending, state->succs[s]);
            }
        }
    }

    size_t cached = IR_NONE;

    for (size_t u = 0; u < state->use_count && u < *first_bad; ++u) {
        const Exposed_Use *use = &state->uses[u];

        if (use->tracked < low || use->tracked >= high) {
            continue;
        }

        if (use->block != cached) {
            compute_in(state, out, words, use->block, in);
            cached = use->block;
        }

        if (!test_bit(in, use->tracked - low)) {
            *first_bad = u;
            return;
        }
    }
}

/* The dense check: the bitset dataflow over every tracked vreg, in
 * slices that fit VERIFY_BITSET_BUDGET. Returns false when out of
 * memory. */
static bool solve_slices(const Def_Use *state, size_t *first_bad) {
    const size_t total_words = (state->tracked + 63) / 64;
    size_t words = VERIFY_BITSET_BUDGET / state->reachable;

    if (words == 0) {
        words = 1;
    }

    if (words > total_words) {
        words = total_words;
    }

    uint64_t *out = malloc(state->reachable * words * sizeof(uint64_t));
    uint64_t *in = malloc(words * sizeof(uint64_t));
    uint64_t *pending =
        malloc(((state->reachable + 63) / 64) * sizeof(uint64_t));
    const bool ok = out != NULL && in != NULL && pending != NULL;

    for (size_t low = 0; ok && low < state->tracked; low += words * 64) {
        solve_slice(state, low, words, out, in, pending, first_bad);
    }

    free(out);
    free(in);
    free(pending);
    return ok;
}

/*
 * The sparse check, tried first. A tracked vreg v is not definitely
 * defined at the start of the block at position b exactly when some
 * reachable path from the entry to a predecessor of b passes no block
 * that defines v (the entry never defines a tracked vreg). So for each v
 * this searches backwards from the predecessors of its use blocks,
 * stopping at the blocks that define v; reaching the entry means the use
 * is bad. v's uses are taken in program order and share one visited set
 * (everything behind a visited block was already searched), so each v
 * costs about the size of the region between its definitions and its
 * uses -- small for the code lowering produces, where a value crossing
 * blocks is used right after the short-circuit or if that it crosses.
 *
 * Many vregs with long regions (for example thousands of vregs defined
 * once and used far away) would make this quadratic, so the search
 * stops after a budget proportional to the size of the CFG; the caller
 * then falls back to the bitset dataflow. *first_bad is lowered to the
 * first bad use found; *finished tells whether every vreg was checked.
 * Returns false only when out of memory.
 */
static bool search_uses(const Def_Use *state, size_t *first_bad,
                        bool *finished) {
    const size_t count = state->reachable;
    const size_t tracked = state->tracked;
    const size_t def_total = state->def_start[count];
    size_t *use_start = calloc(tracked + 1, sizeof(size_t));
    size_t *use_list = malloc((state->use_count + 1) * sizeof(size_t));
    size_t *def_block_start = calloc(tracked + 1, sizeof(size_t));
    size_t *def_blocks = malloc((def_total + 1) * sizeof(size_t));
    size_t *fill = malloc((tracked + 1) * sizeof(size_t));
    /* Per position: id + 1 of the vreg being searched that defines it /
     * that has visited it (stamps, so nothing is reset per vreg). */
    size_t *defines = calloc(count, sizeof(size_t));
    size_t *visited = calloc(count, sizeof(size_t));
    size_t *stack = malloc(count * sizeof(size_t));
    bool ok = use_start != NULL && use_list != NULL &&
              def_block_start != NULL && def_blocks != NULL &&
              fill != NULL && defines != NULL && visited != NULL &&
              stack != NULL;
    /* Predecessor edges examined before giving up. */
    size_t budget = 16 * (count + state->pred_start[count]) + 65536;
    bool exhausted = false;

    if (ok) {
        /* Uses and defining blocks grouped by vreg (counting sorts, so
         * each group keeps program order). */
        for (size_t u = 0; u < state->use_count; ++u) {
            ++use_start[state->uses[u].tracked + 1];
        }

        for (size_t d = 0; d < def_total; ++d) {
            ++def_block_start[state->defs[d] + 1];
        }

        for (size_t v = 0; v < tracked; ++v) {
            use_start[v + 1] += use_start[v];
            def_block_start[v + 1] += def_block_start[v];
        }

        memcpy(fill, use_start, (tracked + 1) * sizeof(size_t));

        for (size_t u = 0; u < state->use_count; ++u) {
            use_list[fill[state->uses[u].tracked]++] = u;
        }

        memcpy(fill, def_block_start, (tracked + 1) * sizeof(size_t));

        for (size_t i = 0; i < count; ++i) {
            for (size_t d = state->def_start[i]; d < state->def_start[i + 1];
                 ++d) {
                def_blocks[fill[state->defs[d]]++] = i;
            }
        }
    }

    for (size_t v = 0; ok && !exhausted && v < tracked; ++v) {
        const size_t stamp = v + 1;

        for (size_t d = def_block_start[v]; d < def_block_start[v + 1];
             ++d) {
            defines[def_blocks[d]] = stamp;
        }

        for (size_t k = use_start[v]; k < use_start[v + 1]; ++k) {
            const size_t u = use_list[k];
            const size_t block = state->uses[u].block;
            size_t depth = 0;
            bool bad = false;

            if (u >= *first_bad) {
                break;
            }

            /* Walk the predecessors of 'block', then of every block
             * pushed: the use block itself is not entered, its use comes
             * before any definition it has. */
            size_t edge = state->pred_start[block];
            size_t end = state->pred_start[block + 1];

            for (;;) {
                if (edge == end) {
                    if (depth == 0) {
                        break;
                    }

                    const size_t q = stack[--depth];
                    edge = state->pred_start[q];
                    end = state->pred_start[q + 1];
                    continue;
                }

                const size_t pred = state->preds[edge++];

                if (budget == 0) {
                    exhausted = true;
                    break;
                }

                --budget;

                if (visited[pred] == stamp) {
                    continue;
                }

                visited[pred] = stamp;

                if (pred == 0) {
                    bad = true;
                    break;
                }

                if (defines[pred] != stamp) {
                    stack[depth++] = pred;
                }
            }

            if (exhausted) {
                break;
            }

            if (bad) {
                *first_bad = u;
                break;
            }
        }
    }

    *finished = ok && !exhausted;
    free(use_start);
    free(use_list);
    free(def_block_start);
    free(def_blocks);
    free(fill);
    free(defines);
    free(visited);
    free(stack);
    return ok;
}

static bool verify_definitions(const Ir_Program *program, Diagnostic *error) {
    Def_Use state;
    const Ir_Instruction *undefined = NULL;
    size_t undefined_vreg = 0;

    memset(&state, 0, sizeof(state));
    state.program = program;

    uint64_t *local = calloc(program->vreg_count / 64 + 1, sizeof(uint64_t));
    state.vreg_info = calloc(program->vreg_count + 1, sizeof(size_t));
    bool ok = local != NULL && state.vreg_info != NULL &&
              order_blocks(&state) && build_predecessors(&state) &&
              scan_blocks(&state, local, &undefined);

    if (ok && undefined != NULL) {
        /* An upward-exposed use in the entry: its first such operand. */
        for (size_t i = 0; i < ir_opcode_operand_count(undefined->opcode);
             ++i) {
            if (!test_bit(local, undefined->operands[i])) {
                undefined_vreg = undefined->operands[i];
                break;
            }
        }
    } else if (ok && state.tracked > 0) {
        size_t first_bad = state.use_count;
        bool finished = false;

        ok = search_uses(&state, &first_bad, &finished);

        if (ok && !finished) {
            ok = solve_slices(&state, &first_bad);
        }

        if (ok && first_bad < state.use_count) {
            undefined = state.uses[first_bad].instruction;
            undefined_vreg = state.uses[first_bad].vreg;
        }
    }

    if (!ok) {
        fail(error, program->blocks[0].span,
             "out of memory while verifying IR");
    } else if (undefined != NULL) {
        ok = fail(error, undefined->span,
                  "virtual register %%%zu may be used before it is defined",
                  undefined_vreg);
    }

    free(local);
    free(state.vreg_info);
    free(state.rpo);
    free(state.order);
    free(state.pred_start);
    free(state.preds);
    free(state.succ_start);
    free(state.succs);
    free(state.def_start);
    free(state.defs);
    free(state.uses);
    return ok;
}

bool ir_verify(const Ir_Program *program, Diagnostic *error) {
    diagnostic_clear(error);

    if (program->block_count == 0) {
        return fail(error, (Source_Span){0, 0, 1, 1},
                    "program has no blocks");
    }

    return verify_unique_names(program, error) &&
           verify_structure(program, error) &&
           verify_definitions(program, error);
}
