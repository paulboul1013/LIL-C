#define _POSIX_C_SOURCE 200809L

#include "ir.h"
#include "ir_interp.h"
#include "ir_lower.h"
#include "ir_opt.h"
#include "ir_parse.h"
#include "ir_verify.h"
#include "lil.h"
#include "test_framework.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Unit tests for stage 4 part B: the -O1 passes (docs/spec.md §8.7).
 * The per-pass "input IR -> expected IR" goldens live in tests/golden/ir/
 * (opt_*.ir, run with --passes=NAME); these tests add what goldens cannot
 * express: the verifier after every pass, idempotence of every pass,
 * termination of the fixpoint on random and adversarial IR, behaviour
 * equal to the unoptimized program on the interpreter, and fold
 * semantics equal to the interpreter's.
 */

/* ---- helpers ------------------------------------------------------------ */

static bool parse_text(const char *text, Ir_Program *program,
                       Diagnostic *error) {
    Source source;

    if (!source_init(&source, "test.ir", text, strlen(text))) {
        ir_program_init(program);
        return false;
    }

    const bool ok = ir_parse(&source, program, error);
    source_free(&source);
    return ok;
}

static char *dump_text(const Ir_Program *program) {
    char *buffer = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&buffer, &length);

    if (stream == NULL) {
        return NULL;
    }

    ir_program_dump(stream, program);
    fclose(stream);
    return buffer;
}

static bool lower_text(const char *path, const char *text, size_t length,
                       Ir_Program *program) {
    Lil_Result result;
    bool ok = lil_analyze_source(path, text, length, &result);

    if (ok) {
        ok = ir_lower_program(&result.program, &result.symbols, program);
    } else {
        ir_program_init(program);
    }

    lil_result_free(&result);
    return ok;
}

/* A deep copy (through the §8.3 text format, which round-trips). */
static bool clone(const Ir_Program *program, Ir_Program *copy) {
    Diagnostic error;
    char *text = dump_text(program);
    const bool ok = text != NULL && parse_text(text, copy, &error);

    if (ok) {
        copy->vreg_count = program->vreg_count;
    }

    free(text);
    return ok;
}

static size_t instruction_count(const Ir_Program *program) {
    size_t count = 0;

    for (size_t b = 0; b < program->block_count; ++b) {
        count += program->blocks[b].count;
    }

    return count;
}

typedef struct {
    Ir_Interp_Result result;
    char *output;
    size_t length;
} Run;

static void run(const Ir_Program *program, uint64_t limit, Run *out) {
    FILE *stream = open_memstream(&out->output, &out->length);

    memset(&out->result, 0, sizeof(out->result));

    if (stream == NULL) {
        out->output = NULL;
        out->length = 0;
        return;
    }

    ir_interp_run(program, stream, limit, &out->result);
    fclose(stream);
}

static bool ended(const Run *run) {
    return run->result.status == IR_INTERP_EXITED ||
           run->result.status == IR_INTERP_TRAPPED;
}

/* Prints the program once when the current test has just failed, so a
 * random case can be reproduced from the log. */
static void report(const char *label, const Ir_Program *program) {
    static const char *last_reported;

    if (!test_state.current_failed || last_reported == test_state.current_test) {
        return;
    }

    last_reported = test_state.current_test;
    printf("  (while checking %s)\n", label);

    char *text = dump_text(program);
    printf("%s", text == NULL ? "" : text);
    free(text);
}

/*
 * §5.7: 'after' behaves like 'before' on the interpreter. No pass makes an
 * execution longer (§8.7), so when 'before' ends within 'limit' steps,
 * 'after' ends the same way in at most as many steps; when 'before' runs
 * out of steps, 'after' has got at least as far, so before's output is a
 * prefix of after's.
 */
static void expect_same_behaviour(const Ir_Program *before,
                                  const Ir_Program *after, uint64_t limit,
                                  const char *label) {
    Run a;
    Run b;

    run(before, limit, &a);
    run(after, limit, &b);

    if (ended(&a)) {
        EXPECT_EQ(a.result.status, b.result.status);
        EXPECT_EQ(a.result.exit_code, b.result.exit_code);
        EXPECT_EQ(a.length, b.length);
        EXPECT_TRUE(a.length == b.length &&
                    memcmp(a.output, b.output, a.length) == 0);
        EXPECT_TRUE(b.result.steps <= a.result.steps);
    } else {
        EXPECT_EQ(IR_INTERP_STEP_LIMIT, a.result.status);
        EXPECT_TRUE(a.length <= b.length &&
                    memcmp(a.output, b.output, a.length) == 0);
    }

    report(label, before);
    free(a.output);
    free(b.output);
}

/* The -O1 order (Ir_Pass order). */
static const Ir_Pass every_pass[IR_PASS_COUNT] = {
    IR_PASS_LOADFWD, IR_PASS_COPYPROP, IR_PASS_FOLD, IR_PASS_SIMPLIFY,
    IR_PASS_BRANCH, IR_PASS_DCE, IR_PASS_THREAD, IR_PASS_UNREACHABLE,
    IR_PASS_MERGE,
};

/* Largest number of rounds any check needed (reported at the end). */
static size_t most_rounds;

/*
 * Every property of §8.7 on one valid program:
 *   - each pass alone: the verifier accepts the result, a second run of
 *     the pass changes nothing (idempotence), behaviour is unchanged;
 *   - all passes to a fixpoint: it is reached (termination), the result
 *     verifies, behaves the same, has no more instructions, and -O1
 *     (ir_optimize) on it changes nothing.
 */
static void check_program(const Ir_Program *original, uint64_t limit,
                          const char *label) {
    Diagnostic error;

    for (int p = 0; p < IR_PASS_COUNT; ++p) {
        Ir_Program copy;
        bool changed = false;

        if (!clone(original, &copy)) {
            EXPECT_TRUE(false);
            return;
        }

        EXPECT_TRUE(ir_run_pass(&copy, (Ir_Pass)p, &changed, &error));
        EXPECT_TRUE(ir_verify(&copy, &error));
        EXPECT_STR_EQ("", error.message);

        char *once = dump_text(&copy);
        EXPECT_TRUE(ir_run_pass(&copy, (Ir_Pass)p, &changed, &error));
        EXPECT_FALSE(changed);
        char *twice = dump_text(&copy);
        EXPECT_STR_EQ(once, twice);
        free(once);
        free(twice);

        expect_same_behaviour(original, &copy, limit, ir_pass_name(p));
        report(ir_pass_name(p), original);
        ir_program_free(&copy);
    }

    Ir_Program copy;
    Ir_Opt_Stats stats;

    if (!clone(original, &copy)) {
        EXPECT_TRUE(false);
        return;
    }

    EXPECT_TRUE(ir_optimize_passes(&copy, every_pass, IR_PASS_COUNT, &stats,
                                   &error));
    EXPECT_STR_EQ("", error.message);
    EXPECT_TRUE(stats.reached_fixpoint);
    EXPECT_TRUE(stats.rounds >= 1);

    if (stats.rounds > most_rounds) {
        most_rounds = stats.rounds;
    }

    EXPECT_TRUE(ir_verify(&copy, &error));
    EXPECT_TRUE(instruction_count(&copy) <= instruction_count(original));
    expect_same_behaviour(original, &copy, limit, label);

    char *fixpoint = dump_text(&copy);
    EXPECT_TRUE(ir_optimize(&copy, 1, &error));
    EXPECT_FALSE(diagnostic_is_warning(&error));
    EXPECT_TRUE(ir_verify(&copy, &error));
    char *compacted = dump_text(&copy);
    /* -O1 = compact, fixpoint, compact: running it again is a no-op. */
    EXPECT_TRUE(ir_optimize(&copy, 1, &error));
    EXPECT_FALSE(diagnostic_is_warning(&error));
    char *again = dump_text(&copy);
    EXPECT_STR_EQ(compacted, again);
    expect_same_behaviour(original, &copy, limit, label);
    report(label, original);
    free(fixpoint);
    free(compacted);
    free(again);
    ir_program_free(&copy);
}

/* ---- pass names, level 0, compaction ------------------------------------ */

/* §6.4 --passes: every pass has a name that maps back to it. */
TEST(pass_names_round_trip) {
    for (int p = 0; p < IR_PASS_COUNT; ++p) {
        const char *name = ir_pass_name((Ir_Pass)p);
        Ir_Pass back = IR_PASS_FOLD;

        EXPECT_TRUE(ir_pass_from_name(name, strlen(name), &back));
        EXPECT_EQ(p, (int)back);
    }

    Ir_Pass pass = IR_PASS_FOLD;
    EXPECT_FALSE(ir_pass_from_name("nope", 4, &pass));
    EXPECT_FALSE(ir_pass_from_name("fold", 3, &pass));
    EXPECT_FALSE(ir_pass_from_name("", 0, &pass));
    EXPECT_STR_EQ("fold", ir_pass_name(IR_PASS_FOLD));
    EXPECT_STR_EQ("?", ir_pass_name((Ir_Pass)IR_PASS_COUNT));
}

/* §8.7 -O0 leaves the program exactly as lowered. */
TEST(level_zero_changes_nothing) {
    static const char text[] = "r0 = 1 + 2 * 3; if (0) putc(r0);";
    Ir_Program program;
    Diagnostic error;

    ASSERT_TRUE(lower_text("t.lil", text, strlen(text), &program));
    char *before = dump_text(&program);
    EXPECT_TRUE(ir_optimize(&program, 0, &error));
    char *after = dump_text(&program);
    EXPECT_STR_EQ(before, after);
    EXPECT_TRUE(ir_optimize(&program, 1, &error));
    char *optimized = dump_text(&program);
    EXPECT_STR_EQ("global @r0\n\nentry:\n  %0 = const 7\n"
                  "  store @r0, %0\n  exit %0\n", optimized);
    free(before);
    free(after);
    free(optimized);
    ir_program_free(&program);
}

/* §8.1 -O1 renumbers vregs densely and in order. */
TEST(compact_vregs_keeps_order) {
    Ir_Program program;
    Diagnostic error;

    ASSERT_TRUE(parse_text("entry:\n  %7 = const 1\n  %300 = add %7, %7\n"
                           "  %20 = neg %300\n  exit %20\n",
                           &program, &error));
    EXPECT_EQ(301, program.vreg_count);
    EXPECT_TRUE(ir_compact_vregs(&program, &error));
    EXPECT_EQ(3, program.vreg_count);
    char *text = dump_text(&program);
    EXPECT_STR_EQ("entry:\n  %0 = const 1\n  %2 = add %0, %0\n"
                  "  %1 = neg %2\n  exit %1\n", text);
    EXPECT_TRUE(ir_verify(&program, &error));
    free(text);
    ir_program_free(&program);
}

/* A pass that changes an invalid program is caught by the verifier the
 * driver runs after every pass (debug and test builds). */
TEST(driver_verifies_after_every_pass) {
    Ir_Program program;
    Diagnostic error;
    const Ir_Pass simplify = IR_PASS_SIMPLIFY;

    /* %0 is used before it is defined: invalid input (a stand-in for a
     * buggy pass). simplify rewrites 'add %0, %1' to 'copy %0'. */
    ASSERT_TRUE(parse_text("entry:\n  %1 = const 0\n  %2 = add %0, %1\n"
                           "  exit %2\n", &program, &error));
    EXPECT_FALSE(ir_optimize_passes(&program, &simplify, 1, NULL, &error));
    EXPECT_EQ(DIAGNOSTIC_INTERNAL, error.stage);
    EXPECT_STR_EQ("IR verification failed after pass 'simplify': virtual "
                  "register %0 may be used before it is defined",
                  error.message);
    ir_program_free(&program);

    /* Adjacent value passes run as one step, named with '+'. */
    const Ir_Pass together[] = {IR_PASS_FOLD, IR_PASS_SIMPLIFY};

    ASSERT_TRUE(parse_text("entry:\n  %1 = const 0\n  %2 = add %0, %1\n"
                           "  exit %2\n", &program, &error));
    EXPECT_FALSE(ir_optimize_passes(&program, together, 2, NULL, &error));
    EXPECT_STR_EQ("IR verification failed after pass 'fold+simplify': "
                  "virtual register %0 may be used before it is defined",
                  error.message);
    ir_program_free(&program);
}

/* ---- fold semantics ----------------------------------------------------- */

/*
 * §5.7 the folded value of every value opcode equals the interpreter's
 * result for the unfolded program, for boundary operands (INT64_MAX + 1
 * among them): the program prints all 8 bytes of the result, and a div
 * that traps is neither folded nor deleted.
 */
TEST(fold_matches_the_interpreter) {
    static const int64_t values[] = {
        0, 1, -1, 2, 7, -7, 255, 256, INT64_MAX, INT64_MIN, INT64_MIN + 1,
    };
    static const Ir_Opcode opcodes[] = {
        IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_AND, IR_OR, IR_EQ, IR_NE,
        IR_LT, IR_LE, IR_GT, IR_GE, IR_NEG, IR_NOT, IR_COPY,
    };
    const size_t count = sizeof(values) / sizeof(values[0]);
    size_t checked = 0;

    for (size_t o = 0; o < sizeof(opcodes) / sizeof(opcodes[0]); ++o) {
        for (size_t i = 0; i < count; ++i) {
            for (size_t j = 0; j < count; ++j) {
                const Ir_Opcode opcode = opcodes[o];
                const bool unary = ir_opcode_shape(opcode) == IR_SHAPE_UNARY;
                char text[1024];
                int used = snprintf(
                    text, sizeof(text),
                    "entry:\n  %%0 = const %lld\n  %%1 = const %lld\n"
                    "  %%2 = %s %%0%s\n  %%3 = const 256\n",
                    (long long)values[i], (long long)values[j],
                    ir_opcode_name(opcode), unary ? "" : ", %1");

                for (int byte = 0; byte < 8; ++byte) {
                    used += snprintf(text + used, sizeof(text) - (size_t)used,
                                     "  putc %%2\n  %%2 = div %%2, %%3\n");
                }

                snprintf(text + used, sizeof(text) - (size_t)used,
                         "  exit %%2\n");

                Ir_Program unfolded;
                Ir_Program folded;
                Diagnostic error;
                bool changed = false;

                if (!parse_text(text, &unfolded, &error) ||
                    !clone(&unfolded, &folded)) {
                    EXPECT_STR_EQ("(parses)", error.message);
                    return;
                }

                EXPECT_TRUE(ir_run_pass(&folded, IR_PASS_FOLD, &changed,
                                        &error));
                EXPECT_TRUE(ir_verify(&folded, &error));

                int64_t expected = 0;
                const bool traps =
                    unary ? false
                          : !ir_eval_binary(opcode, values[i], values[j],
                                            &expected);
                const Ir_Instruction *result =
                    &folded.blocks[0].instructions[2];

                if (traps) {
                    EXPECT_EQ(IR_DIV, result->opcode);
                } else {
                    if (unary) {
                        expected = ir_eval_unary(opcode, values[i]);
                    }

                    EXPECT_EQ(IR_CONST, result->opcode);
                    EXPECT_EQ(expected, result->value);
                }

                expect_same_behaviour(&unfolded, &folded, 1000, text);
                ir_program_free(&unfolded);
                ir_program_free(&folded);
                ++checked;
            }
        }
    }

    EXPECT_TRUE(checked > 1000);
}

/* §5.7 INT64_MAX + 1 folds to INT64_MIN, exactly what the interpreter
 * computes for the unfolded program (exit status (INT64_MIN + 3) & 0xff). */
TEST(fold_int64_max_plus_one_wraps) {
    static const char text[] = "r0 = 9223372036854775807 + 1 + 3;";
    Ir_Program program;
    Ir_Program optimized;
    Diagnostic error;

    ASSERT_TRUE(lower_text("t.lil", text, strlen(text), &program));

    if (clone(&program, &optimized)) {
        EXPECT_TRUE(ir_optimize(&optimized, 1, &error));
        EXPECT_EQ(IR_CONST, optimized.blocks[0].instructions[0].opcode);
        EXPECT_EQ(INT64_MIN + 3, optimized.blocks[0].instructions[0].value);

        Run a;
        Run b;
        run(&program, 1000, &a);
        run(&optimized, 1000, &b);
        EXPECT_EQ(IR_INTERP_EXITED, a.result.status);
        EXPECT_EQ(INT64_MIN + 3, a.result.exit_value);
        EXPECT_EQ(a.result.exit_value, b.result.exit_value);
        EXPECT_EQ(3, b.result.exit_code);
        free(a.output);
        free(b.output);
        ir_program_free(&optimized);
    }

    ir_program_free(&program);
}

/* ---- every golden, example and acceptance program ------------------------ */

static bool has_suffix(const char *name, const char *suffix) {
    const size_t length = strlen(name);
    const size_t suffix_length = strlen(suffix);

    return length >= suffix_length &&
           strcmp(name + length - suffix_length, suffix) == 0;
}

static char *read_whole_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    char *buffer = NULL;
    size_t capacity = 0;

    *length = 0;

    if (file == NULL) {
        return NULL;
    }

    for (;;) {
        if (capacity - *length < 4096) {
            char *grown = realloc(buffer, capacity + 65536);

            if (grown == NULL) {
                break;
            }

            buffer = grown;
            capacity += 65536;
        }

        const size_t got = fread(buffer + *length, 1, capacity - *length - 1,
                                 file);
        *length += got;

        if (got == 0) {
            break;
        }
    }

    fclose(file);

    if (buffer != NULL) {
        buffer[*length] = '\0';
    }

    return buffer;
}

/* Lowered .lil programs and valid .ir files of one directory. */
static size_t check_directory(const char *directory) {
    DIR *dir = opendir(directory);
    size_t checked = 0;

    if (dir == NULL) {
        return 0;
    }

    for (struct dirent *entry = readdir(dir); entry != NULL;
         entry = readdir(dir)) {
        const bool is_lil = has_suffix(entry->d_name, ".lil");

        if (!is_lil && !has_suffix(entry->d_name, ".ir")) {
            continue;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

        size_t length = 0;
        char *text = read_whole_file(path, &length);
        Ir_Program program;
        Diagnostic error;
        bool ok = text != NULL;

        if (ok && is_lil) {
            ok = lower_text(path, text, length, &program);
        } else if (ok) {
            ok = parse_text(text, &program, &error) &&
                 ir_verify(&program, &error);
        } else {
            ir_program_init(&program);
        }

        if (ok) {
            /* Enough for every acceptance program; loops beyond that
             * are checked by the prefix rule. */
            check_program(&program, 2000000, path);
            ++checked;
        }

        ir_program_free(&program);
        free(text);
    }

    closedir(dir);
    return checked;
}

TEST(every_golden_and_example_optimizes_correctly) {
    static const char *const directories[] = {
        "tests/golden/ir", "tests/golden/interp", "tests/golden/ast",
        "examples", "examples/accept",
    };
    size_t checked = 0;

    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]);
         ++i) {
        checked += check_directory(directories[i]);
    }

    EXPECT_TRUE(checked >= 100);
}

/* ---- random IR ---------------------------------------------------------- */

typedef struct {
    uint64_t state;
} Rng;

static uint64_t rng_next(Rng *rng) {
    /* xorshift64* */
    rng->state ^= rng->state >> 12;
    rng->state ^= rng->state << 25;
    rng->state ^= rng->state >> 27;
    return rng->state * UINT64_C(2685821657736338717);
}

static size_t below(Rng *rng, size_t n) {
    return (size_t)(rng_next(rng) % n);
}

static int64_t interesting(Rng *rng) {
    static const int64_t values[] = {
        0, 1, -1, 2, 3, 7, -7, 255, 256, INT64_MAX, INT64_MIN,
    };

    return values[below(rng, sizeof(values) / sizeof(values[0]))];
}

/*
 * A random valid program: the entry block first defines every vreg (so
 * every use anywhere is defined, §8.4), then every block gets random
 * instructions -- redefining vregs (not SSA), dividing by anything,
 * loading and storing -- and a random terminator. Empty forwarding
 * blocks, cycles of them, constant and same-target branches, unreachable
 * blocks and infinite loops all come up.
 */
static void random_program(Rng *rng, Ir_Program *program) {
    static const Ir_Opcode binary[] = {
        IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_AND, IR_OR, IR_EQ, IR_NE,
        IR_LT, IR_LE, IR_GT, IR_GE,
    };
    static const Ir_Opcode unary[] = {IR_COPY, IR_NEG, IR_NOT};
    const size_t globals = 1 + below(rng, 3);
    const size_t blocks = 1 + below(rng, 10);
    const size_t vregs = 1 + below(rng, 8);
    Source_Span span;

    memset(&span, 0, sizeof(span));
    ir_program_init(program);

    for (size_t g = 0; g < globals; ++g) {
        char name[32];
        size_t id = 0;
        snprintf(name, sizeof(name), "g%zu", g);
        ir_program_add_global(program, name, strlen(name), &id);
    }

    for (size_t b = 0; b < blocks; ++b) {
        char name[32];
        size_t index = 0;
        snprintf(name, sizeof(name), b == 0 ? "entry" : "b%zu", b);
        ir_program_add_block(program, name, strlen(name), span, &index);
    }

    program->vreg_count = vregs;

    for (size_t v = 0; v < vregs; ++v) {
        Ir_Instruction instruction =
            below(rng, 4) == 0
                ? ir_make_load(v, below(rng, globals), span)
                : ir_make_const(v, interesting(rng), span);
        ir_block_append(&program->blocks[0], &instruction);
    }

    for (size_t b = 0; b < blocks; ++b) {
        Ir_Block *block = &program->blocks[b];
        const size_t count = below(rng, 3) == 0 ? 0 : below(rng, 8);

        for (size_t k = 0; k < count; ++k) {
            const size_t dest = below(rng, vregs);
            const size_t a = below(rng, vregs);
            const size_t c = below(rng, vregs);
            const size_t kind = below(rng, 12);
            Ir_Instruction instruction;

            if (kind < 2) {
                instruction = ir_make_const(dest, interesting(rng), span);
            } else if (kind < 6) {
                instruction = ir_make_binary(
                    binary[below(rng, sizeof(binary) / sizeof(binary[0]))],
                    dest, a, c, span);
            } else if (kind < 8) {
                instruction = ir_make_unary(
                    unary[below(rng, sizeof(unary) / sizeof(unary[0]))],
                    dest, a, span);
            } else if (kind < 9) {
                instruction = ir_make_load(dest, below(rng, globals), span);
            } else if (kind < 11) {
                instruction = ir_make_store(below(rng, globals), a, span);
            } else {
                instruction = ir_make_putc(a, span);
            }

            ir_block_append(block, &instruction);
        }

        const size_t kind = below(rng, 10);
        const size_t target = below(rng, blocks);
        Ir_Instruction last;

        if (kind < 4) {
            last = ir_make_jmp(target, span);
        } else if (kind < 8) {
            last = ir_make_br(below(rng, vregs), target,
                              below(rng, 4) == 0 ? target
                                                 : below(rng, blocks),
                              span);
        } else {
            last = ir_make_exit(below(rng, vregs), span);
        }

        ir_block_append(block, &last);
    }
}

/* §8.7 on 3000 random programs: verifier after every pass, idempotence,
 * termination, unchanged behaviour. */
TEST(random_programs_keep_every_property) {
    Rng rng = {UINT64_C(0x2026100915370001)};
    Diagnostic error;

    for (int i = 0; i < 3000; ++i) {
        Ir_Program program;
        char label[64];

        random_program(&rng, &program);
        snprintf(label, sizeof(label), "random program %d", i);

        if (!ir_verify(&program, &error)) {
            EXPECT_STR_EQ("(valid)", error.message);
            report(label, &program);
        } else {
            check_program(&program, 400, label);
        }

        ir_program_free(&program);
    }
}

/* §8.7 any order and subset of the passes reaches a fixpoint that
 * verifies and behaves the same (the passes do not depend on running in
 * the -O1 order). */
TEST(random_pass_orders_reach_a_fixpoint) {
    Rng rng = {UINT64_C(0x5eed00004c494c43)};
    Diagnostic error;

    for (int i = 0; i < 1500; ++i) {
        Ir_Program program;
        Ir_Program copy;
        Ir_Pass passes[12];
        const size_t count = 1 + below(&rng, 12);
        Ir_Opt_Stats stats;

        random_program(&rng, &program);

        for (size_t k = 0; k < count; ++k) {
            passes[k] = (Ir_Pass)below(&rng, IR_PASS_COUNT);
        }

        if (clone(&program, &copy)) {
            EXPECT_TRUE(ir_optimize_passes(&copy, passes, count, &stats,
                                           &error));
            EXPECT_STR_EQ("", error.message);
            EXPECT_TRUE(stats.reached_fixpoint);
            EXPECT_TRUE(ir_verify(&copy, &error));
            expect_same_behaviour(&program, &copy, 400, "random order");
            ir_program_free(&copy);
        }

        ir_program_free(&program);
    }
}

/* ---- adversarial IR ----------------------------------------------------- */

static double seconds_since(clock_t start) {
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

/* Builds "entry: jmp f1; f1: jmp f2; ...; f(n-1): jmp last; last: exit"
 * with the blocks in forward or reverse index order. */
static void forwarding_chain(Ir_Program *program, size_t n, bool reverse) {
    Source_Span span;
    size_t vreg = 0;

    memset(&span, 0, sizeof(span));
    ir_program_init(program);

    for (size_t i = 0; i <= n; ++i) {
        char name[32];
        size_t index = 0;
        snprintf(name, sizeof(name), i == 0 ? "entry" : "f%zu", i);
        ir_program_add_block(program, name, strlen(name), span, &index);
    }

    vreg = ir_program_new_vreg(program);

    /* Chain position p (0 = entry) lives in block position_block(p). */
    for (size_t p = 0; p <= n; ++p) {
        const size_t block = p == 0 || !reverse ? p : n + 1 - p;
        const size_t next = p + 1 > n ? 0 : (!reverse ? p + 1 : n - p);
        Ir_Block *target = &program->blocks[block];

        if (p == n) {
            Ir_Instruction value = ir_make_const(vreg, 42, span);
            Ir_Instruction exit = ir_make_exit(vreg, span);
            ir_block_append(target, &value);
            ir_block_append(target, &exit);
        } else {
            Ir_Instruction jump = ir_make_jmp(next, span);
            ir_block_append(target, &jump);
        }
    }
}

/* §8.7 thread/merge/unreachable stay linear on 100000-block chains in
 * either order, and the result is one block. */
TEST(long_chains_optimize_quickly) {
    for (int reverse = 0; reverse <= 1; ++reverse) {
        Ir_Program program;
        Diagnostic error;
        Ir_Opt_Stats stats;

        forwarding_chain(&program, 100000, reverse != 0);
        ASSERT_TRUE(ir_verify(&program, &error));

        const clock_t start = clock();
        EXPECT_TRUE(ir_optimize_passes(&program, every_pass, IR_PASS_COUNT,
                                       &stats, &error));
        EXPECT_TRUE(seconds_since(start) < 5.0);
        EXPECT_TRUE(stats.reached_fixpoint);
        EXPECT_TRUE(stats.rounds <= 3);
        EXPECT_EQ(1, program.block_count);

        Run result;
        run(&program, 10, &result);
        EXPECT_EQ(IR_INTERP_EXITED, result.result.status);
        EXPECT_EQ(42, result.result.exit_code);
        free(result.output);
        ir_program_free(&program);
    }
}

/*
 * §8.7 cycles of empty blocks: the passes terminate, keep the infinite
 * loop an infinite loop (still running at the step limit) and keep what
 * is reachable from it valid.
 */
TEST(empty_block_cycles_terminate) {
    static const char *const programs[] = {
        "entry:\n  jmp entry\n",
        "entry:\n  jmp a\na:\n  jmp a\n",
        "entry:\n  jmp a\na:\n  jmp b\nb:\n  jmp a\n",
        "entry:\n  jmp a\na:\n  jmp b\nb:\n  jmp c\nc:\n  jmp a\n",
        "global @g\nentry:\n  %0 = load @g\n  br %0, a, b\na:\n  jmp b\n"
        "b:\n  jmp a\n",
        "entry:\n  %0 = const 1\n  br %0, a, a\na:\n  jmp entry\n",
        "entry:\n  jmp a\na:\n  jmp b\nb:\n  jmp a\nx:\n  jmp x\n"
        "y:\n  %0 = const 1\n  exit %0\n",
        "global @g\nentry:\n  %0 = load @g\n  br %0, loop, out\n"
        "loop:\n  jmp loop2\nloop2:\n  jmp loop\nout:\n  exit %0\n",
    };

    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i) {
        Ir_Program program;
        Diagnostic error;

        ASSERT_TRUE(parse_text(programs[i], &program, &error));
        ASSERT_TRUE(ir_verify(&program, &error));
        check_program(&program, 100, programs[i]);
        ir_program_free(&program);
    }

    /* A big cycle reached from the entry. */
    Ir_Program program;
    Diagnostic error;
    Ir_Opt_Stats stats;

    forwarding_chain(&program, 50000, false);
    /* Close the chain: the last block jumps back to f1 instead of
     * exiting. */
    Ir_Block *last = &program.blocks[program.block_count - 1];
    last->count = 0;
    Ir_Instruction back = ir_make_jmp(1, last->span);
    ir_block_append(last, &back);
    ASSERT_TRUE(ir_verify(&program, &error));
    EXPECT_TRUE(ir_optimize_passes(&program, every_pass, IR_PASS_COUNT,
                                   &stats, &error));
    EXPECT_TRUE(stats.reached_fixpoint);
    EXPECT_TRUE(ir_verify(&program, &error));

    Run result;
    run(&program, 1000, &result);
    EXPECT_EQ(IR_INTERP_STEP_LIMIT, result.result.status);
    free(result.output);
    ir_program_free(&program);
}

/*
 * §8.7 a ladder of 2000 diamonds where each join adds to a constant from
 * the previous one: folding follows the reverse postorder, so the whole
 * ladder folds in a few rounds, not one round per diamond.
 */
TEST(constant_ladder_folds_in_few_rounds) {
    char *text = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&text, &length);

    ASSERT_TRUE(stream != NULL);
    /* %0 = 1, %1 = the branch condition, %2 = 0; diamond i adds %0 to
     * %(i + 1) giving %(i + 2) in its join block. */
    fprintf(stream, "global @g\nentry:\n  %%0 = const 1\n  %%1 = load @g\n"
                    "  %%2 = const 0\n  jmp d1\n");

    for (int i = 1; i <= 2000; ++i) {
        fprintf(stream,
                "d%d:\n  br %%1, l%d, r%d\nl%d:\n  putc %%1\n  jmp j%d\n"
                "r%d:\n  jmp j%d\nj%d:\n  %%%d = add %%%d, %%0\n"
                "  jmp d%d\n",
                i, i, i, i, i, i, i, i, i + 2, i + 1, i + 1);
    }

    fprintf(stream, "d2001:\n  exit %%2002\n");
    fclose(stream);

    Ir_Program program;
    Diagnostic error;
    Ir_Opt_Stats stats;

    if (parse_text(text, &program, &error)) {
        EXPECT_TRUE(ir_verify(&program, &error));
        EXPECT_STR_EQ("", error.message);
        EXPECT_TRUE(ir_optimize_passes(&program, every_pass, IR_PASS_COUNT,
                                       &stats, &error));
        EXPECT_TRUE(stats.reached_fixpoint);
        EXPECT_TRUE(stats.rounds <= 4);
        EXPECT_TRUE(ir_verify(&program, &error));

        Run result;
        run(&program, 1000000, &result);
        EXPECT_EQ(IR_INTERP_EXITED, result.result.status);
        EXPECT_EQ(2000 & 0xff, result.result.exit_code);
        free(result.output);
    } else {
        EXPECT_STR_EQ("(parses)", error.message);
    }

    ir_program_free(&program);
    free(text);
}

/* §8.7 copyprop resolves a long copy chain in one block at once and dce
 * then removes it: few rounds. */
TEST(long_copy_chain_in_one_block) {
    Ir_Program program;
    Diagnostic error;
    Ir_Opt_Stats stats;
    Source_Span span;

    memset(&span, 0, sizeof(span));
    ir_program_init(&program);
    size_t entry = 0;
    size_t global = 0;
    ASSERT_TRUE(ir_program_add_global(&program, "g", 1, &global));
    ASSERT_TRUE(ir_program_add_block(&program, "entry", 5, span, &entry));

    Ir_Instruction load = ir_make_load(ir_program_new_vreg(&program), global,
                                       span);
    ir_block_append(&program.blocks[0], &load);

    for (size_t i = 1; i < 50000; ++i) {
        Ir_Instruction copy =
            ir_make_unary(IR_COPY, ir_program_new_vreg(&program), i - 1, span);
        ir_block_append(&program.blocks[0], &copy);
    }

    Ir_Instruction exit = ir_make_exit(program.vreg_count - 1, span);
    ir_block_append(&program.blocks[0], &exit);
    ASSERT_TRUE(ir_verify(&program, &error));

    EXPECT_TRUE(ir_optimize_passes(&program, every_pass, IR_PASS_COUNT,
                                   &stats, &error));
    EXPECT_TRUE(stats.rounds <= 3);
    EXPECT_EQ(2, program.blocks[0].count);
    ir_program_free(&program);
}

/* ---- convergence --------------------------------------------------------- */

/* 'x = 1;' then n lines 'if (x == i) x = i + 1; else putc('E');', then
 * 'r0 = x;' (the reviewers' ladder). */
static char *if_ladder(size_t n) {
    char *text = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&text, &length);

    if (stream == NULL) {
        return NULL;
    }

    fprintf(stream, "x = 1;\n");

    for (size_t i = 1; i <= n; ++i) {
        fprintf(stream, "if (x == %zu) x = %zu; else putc('E');\n", i,
                i + 1);
    }

    fprintf(stream, "r0 = x;\n");
    fclose(stream);
    return text;
}

/*
 * §8.7 a ladder of constant ifs in LIL folds in one sweep: branch kills
 * each else block, so the join is chained to the then block and the next
 * load is forwarded. The fixpoint takes 2 rounds whatever the length
 * (it took 2 per if before), is one block, and is a real fixpoint: -O1
 * on its dump (through --ir-input's parser) changes nothing.
 */
TEST(lil_if_ladder_folds_in_two_rounds) {
    static const size_t sizes[] = {500, 2000};

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        const size_t n = sizes[s];
        char *text = if_ladder(n);
        Ir_Program program;
        Ir_Program copy;
        Diagnostic error;
        Ir_Opt_Stats stats;

        ASSERT_TRUE(text != NULL);
        ASSERT_TRUE(lower_text("ladder.lil", text, strlen(text), &program));
        ASSERT_TRUE(clone(&program, &copy));

        const clock_t start = clock();
        EXPECT_TRUE(ir_optimize_passes(&copy, every_pass, IR_PASS_COUNT,
                                       &stats, &error));
        EXPECT_TRUE(seconds_since(start) < 2.0);
        EXPECT_TRUE(stats.reached_fixpoint);
        EXPECT_TRUE(stats.rounds <= 2);
        EXPECT_FALSE(diagnostic_is_warning(&error));
        EXPECT_EQ(1, copy.block_count);
        expect_same_behaviour(&program, &copy, 1000000, "if ladder");
        ir_program_free(&copy);

        /* -O1, dump, parse, -O1 again: the same text. */
        EXPECT_TRUE(ir_optimize(&program, 1, &error));
        EXPECT_FALSE(diagnostic_is_warning(&error));
        char *once = dump_text(&program);
        Ir_Program reparsed;

        if (once != NULL && parse_text(once, &reparsed, &error)) {
            EXPECT_TRUE(ir_optimize(&reparsed, 1, &error));
            char *twice = dump_text(&reparsed);
            EXPECT_STR_EQ(once, twice);
            free(twice);
            ir_program_free(&reparsed);
        } else {
            EXPECT_TRUE(false);
        }

        Run result;
        run(&program, 1000000, &result);
        EXPECT_EQ(IR_INTERP_EXITED, result.result.status);
        EXPECT_EQ((int)((n + 1) & 0xff), result.result.exit_code);
        EXPECT_EQ(0, result.length);
        free(result.output);
        free(once);
        ir_program_free(&program);
        free(text);
    }
}

/*
 * §8.7 hitting the round limit is reported, not hidden: with branch and
 * fold in separate steps a ladder needs a round per if, so a limit of 2
 * rounds stops early. The result is still valid and behaves the same,
 * reached_fixpoint is false and *error holds an internal warning. With
 * the real limit the same list reaches the fixpoint.
 */
TEST(round_limit_is_reported) {
    static const Ir_Pass passes[] = {IR_PASS_BRANCH, IR_PASS_DCE,
                                     IR_PASS_LOADFWD, IR_PASS_FOLD};
    char *text = if_ladder(6);
    Ir_Program program;
    Ir_Program copy;
    Diagnostic error;
    Ir_Opt_Stats stats;

    ASSERT_TRUE(text != NULL);
    ASSERT_TRUE(lower_text("ladder.lil", text, strlen(text), &program));
    ASSERT_TRUE(clone(&program, &copy));

    EXPECT_TRUE(ir_optimize_passes_bounded(&copy, passes, 4, 2, &stats,
                                           &error));
    EXPECT_FALSE(stats.reached_fixpoint);
    EXPECT_EQ(2, stats.rounds);
    EXPECT_TRUE(diagnostic_is_warning(&error));
    EXPECT_EQ(DIAGNOSTIC_INTERNAL, error.stage);
    EXPECT_STR_EQ("the optimizer stopped after 2 rounds without reaching a "
                  "fixpoint", error.message);
    EXPECT_TRUE(ir_verify(&copy, &error));
    expect_same_behaviour(&program, &copy, 100000, "round limit");
    ir_program_free(&copy);

    ASSERT_TRUE(clone(&program, &copy));
    EXPECT_TRUE(ir_optimize_passes(&copy, passes, 4, &stats, &error));
    EXPECT_TRUE(stats.reached_fixpoint);
    EXPECT_TRUE(stats.rounds > 6);
    EXPECT_STR_EQ("", error.message);
    expect_same_behaviour(&program, &copy, 100000, "round limit");
    ir_program_free(&copy);
    ir_program_free(&program);
    free(text);
}

/* The reviewers' e1 shape: unreachable blocks u1..un, each using the
 * vreg the next one defines, listed against the dependency order; un
 * defines %n = const 1. */
static char *reversed_unreachable_chain(size_t n) {
    char *text = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&text, &length);

    if (stream == NULL) {
        return NULL;
    }

    fprintf(stream, "entry:\n  %%0 = const 0\n  exit %%0\n");

    for (size_t i = 1; i < n; ++i) {
        fprintf(stream, "u%zu:\n  %%%zu = neg %%%zu\n  putc %%%zu\n"
                        "  jmp u%zu\n", i, i, i + 1, i, i);
    }

    fprintf(stream, "u%zu:\n  %%%zu = const 1\n  jmp u%zu\n", n, n, n);
    fclose(stream);
    return text;
}

/*
 * §8.7 the value passes do not visit unreachable blocks: a reversed
 * dependency chain there is left exactly as it is (fold used to need a
 * run per block, up to the old limit of 64, and was quadratic: 70000
 * blocks took over 600 s), and running the pass again changes nothing.
 */
TEST(fold_ignores_unreachable_chains) {
    static const size_t sizes[] = {70, 70000};

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        char *text = reversed_unreachable_chain(sizes[s]);
        Ir_Program program;
        Diagnostic error;
        bool changed = true;

        ASSERT_TRUE(text != NULL);
        ASSERT_TRUE(parse_text(text, &program, &error));
        ASSERT_TRUE(ir_verify(&program, &error));
        char *before = dump_text(&program);

        for (int p = 0; p < IR_PASS_COUNT; ++p) {
            if (p == IR_PASS_DCE || p == IR_PASS_THREAD ||
                p == IR_PASS_UNREACHABLE || p == IR_PASS_MERGE) {
                continue;
            }

            const clock_t start = clock();
            EXPECT_TRUE(ir_run_pass(&program, (Ir_Pass)p, &changed, &error));
            EXPECT_TRUE(seconds_since(start) < 2.0);
            EXPECT_FALSE(changed);
        }

        char *after = dump_text(&program);
        EXPECT_STR_EQ(before, after);
        EXPECT_TRUE(ir_run_pass(&program, IR_PASS_FOLD, &changed, &error));
        EXPECT_FALSE(changed);
        free(before);
        free(after);
        ir_program_free(&program);
        free(text);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        test_set_filter(argv[1]);
    }

    RUN_TEST(pass_names_round_trip);
    RUN_TEST(level_zero_changes_nothing);
    RUN_TEST(compact_vregs_keeps_order);
    RUN_TEST(driver_verifies_after_every_pass);
    RUN_TEST(fold_matches_the_interpreter);
    RUN_TEST(fold_int64_max_plus_one_wraps);
    RUN_TEST(every_golden_and_example_optimizes_correctly);
    RUN_TEST(random_programs_keep_every_property);
    RUN_TEST(random_pass_orders_reach_a_fixpoint);
    RUN_TEST(long_chains_optimize_quickly);
    RUN_TEST(empty_block_cycles_terminate);
    RUN_TEST(constant_ladder_folds_in_few_rounds);
    RUN_TEST(long_copy_chain_in_one_block);
    RUN_TEST(lil_if_ladder_folds_in_two_rounds);
    RUN_TEST(round_limit_is_reported);
    RUN_TEST(fold_ignores_unreachable_chains);
    printf("most rounds to reach a fixpoint: %zu\n", most_rounds);
    return test_finish();
}
