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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Unit tests for stage 4 part A: the IR (docs/spec.md §8), lowering,
 * the text format, the verifier and the interpreter. Each test names the
 * spec clause it checks.
 */

/* ---- helpers ------------------------------------------------------------ */

/* Front end + lowering of a NUL-terminated LIL program. */
static bool lower_text(const char *text, Ir_Program *program) {
    Lil_Result result;
    bool ok = lil_analyze_source("test.lil", text, strlen(text), &result);

    if (ok) {
        ok = ir_lower_program(&result.program, &result.symbols, program);
    } else {
        ir_program_init(program);
    }

    lil_result_free(&result);
    return ok;
}

/* ir_parse() of a NUL-terminated IR text. */
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

/* The §8.3 dump as a malloc'ed string. */
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

/* Runs a program; its putc output is returned (malloc'ed) in *output. */
static void run_program(const Ir_Program *program, uint64_t limit,
                        Ir_Interp_Result *result, char **output) {
    size_t length = 0;
    FILE *stream = open_memstream(output, &length);

    if (stream == NULL) {
        *output = NULL;
        memset(result, 0, sizeof(*result));
        return;
    }

    ir_interp_run(program, stream, limit, result);
    fclose(stream);
}

/* Lowers, verifies and runs a LIL program; checks output and status. */
static void expect_run(const char *text, const char *expected_output,
                       Ir_Interp_Status expected_status, int expected_code) {
    Ir_Program program;
    Diagnostic error;

    if (!lower_text(text, &program)) {
        EXPECT_TRUE(false);
        return;
    }

    EXPECT_TRUE(ir_verify(&program, &error));

    Ir_Interp_Result result;
    char *output = NULL;
    run_program(&program, 1000000, &result, &output);
    EXPECT_STR_EQ(expected_output, output);
    EXPECT_EQ(expected_status, result.status);

    if (expected_status == IR_INTERP_EXITED) {
        EXPECT_EQ(expected_code, result.exit_code);
    }

    free(output);
    ir_program_free(&program);
}

/* Expects ir_verify() of the IR text to fail at line:column with
 * 'message' (the text must parse). */
static void expect_verify_error(const char *text, size_t line, size_t column,
                                const char *message) {
    Ir_Program program;
    Diagnostic error;

    if (!parse_text(text, &program, &error)) {
        EXPECT_STR_EQ("(parses)", error.message);
        return;
    }

    EXPECT_FALSE(ir_verify(&program, &error));
    EXPECT_EQ(DIAGNOSTIC_VERIFY, error.stage);
    EXPECT_EQ(line, error.span.line);
    EXPECT_EQ(column, error.span.column);
    EXPECT_STR_EQ(message, error.message);
    ir_program_free(&program);
}

static void expect_parse_error(const char *text, size_t line, size_t column,
                               const char *message) {
    Ir_Program program;
    Diagnostic error;

    EXPECT_FALSE(parse_text(text, &program, &error));
    EXPECT_EQ(DIAGNOSTIC_IR, error.stage);
    EXPECT_EQ(line, error.span.line);
    EXPECT_EQ(column, error.span.column);
    EXPECT_STR_EQ(message, error.message);
    EXPECT_EQ(0, program.block_count);
    ir_program_free(&program);
}

static void expect_valid(const char *text) {
    Ir_Program program;
    Diagnostic error;

    ASSERT_TRUE(parse_text(text, &program, &error));
    EXPECT_TRUE(ir_verify(&program, &error));
    EXPECT_STR_EQ("", error.message);
    ir_program_free(&program);
}

/* ---- the IR itself ---------------------------------------------------- */

/* §8.2 the opcode table: names, shapes and operand counts. */
TEST(ir_opcode_table_is_consistent) {
    for (int i = 0; i < IR_OPCODE_COUNT; ++i) {
        const Ir_Opcode opcode = (Ir_Opcode)i;
        const char *name = ir_opcode_name(opcode);
        Ir_Opcode parsed;

        EXPECT_TRUE(ir_opcode_from_name(name, strlen(name), &parsed));
        EXPECT_EQ(opcode, parsed);
        EXPECT_EQ(ir_opcode_target_count(opcode) > 0 || opcode == IR_EXIT,
                  ir_opcode_is_terminator(opcode));
    }

    Ir_Opcode parsed;
    EXPECT_FALSE(ir_opcode_from_name("mod", 3, &parsed));
    EXPECT_FALSE(ir_opcode_from_name("ad", 2, &parsed));
    EXPECT_STR_EQ("?", ir_opcode_name((Ir_Opcode)99));
    EXPECT_FALSE(ir_opcode_has_dest((Ir_Opcode)99));
    EXPECT_EQ(1, ir_opcode_operand_count((Ir_Opcode)99));

    EXPECT_EQ(2, ir_opcode_operand_count(IR_DIV));
    EXPECT_EQ(2, ir_opcode_operand_count(IR_GE));
    EXPECT_EQ(1, ir_opcode_operand_count(IR_NOT));
    EXPECT_EQ(1, ir_opcode_operand_count(IR_STORE));
    EXPECT_EQ(1, ir_opcode_operand_count(IR_BR));
    EXPECT_EQ(0, ir_opcode_operand_count(IR_CONST));
    EXPECT_EQ(0, ir_opcode_operand_count(IR_LOAD));
    EXPECT_EQ(0, ir_opcode_operand_count(IR_JMP));
    EXPECT_TRUE(ir_opcode_has_dest(IR_LOAD));
    EXPECT_TRUE(ir_opcode_has_dest(IR_CONST));
    EXPECT_FALSE(ir_opcode_has_dest(IR_STORE));
    EXPECT_FALSE(ir_opcode_has_dest(IR_PUTC));
    EXPECT_EQ(2, ir_opcode_target_count(IR_BR));
    EXPECT_EQ(1, ir_opcode_target_count(IR_JMP));
    EXPECT_EQ(0, ir_opcode_target_count(IR_EXIT));
}

/* §8.1 building a program through the API; lookups; free is repeatable. */
TEST(ir_program_api_builds_and_frees) {
    Ir_Program program;
    const Source_Span span = {0, 0, 1, 1};
    size_t id = 0;
    size_t block = 0;

    ir_program_init(&program);
    EXPECT_TRUE(ir_program_add_global(&program, "xy", 1, &id));
    EXPECT_EQ(0, id);
    EXPECT_STR_EQ("x", program.globals[0].name);

    for (int i = 0; i < 20; ++i) {
        char name[16];
        const int length = snprintf(name, sizeof(name), "b%d", i);
        EXPECT_TRUE(ir_program_add_block(&program, name, (size_t)length,
                                         span, &block));
        EXPECT_EQ((size_t)i, block);
    }

    EXPECT_EQ(19, ir_program_find_block(&program, "b19", 3));
    EXPECT_EQ(IR_NONE, ir_program_find_block(&program, "b1x", 3));
    EXPECT_EQ(0, ir_program_find_global(&program, "x", 1));
    EXPECT_EQ(IR_NONE, ir_program_find_global(&program, "y", 1));

    EXPECT_TRUE(ir_block_terminator(&program.blocks[0]) == NULL);
    const size_t v = ir_program_new_vreg(&program);
    const Ir_Instruction c = ir_make_const(v, 5, span);
    EXPECT_TRUE(ir_block_append(&program.blocks[0], &c));
    EXPECT_TRUE(ir_block_terminator(&program.blocks[0]) == NULL);
    const Ir_Instruction e = ir_make_exit(v, span);
    EXPECT_TRUE(ir_block_append(&program.blocks[0], &e));
    EXPECT_TRUE(ir_block_terminator(&program.blocks[0]) != NULL);
    EXPECT_EQ(1, program.vreg_count);

    ir_program_free(&program);
    ir_program_free(&program);
    EXPECT_EQ(0, program.block_count);
    EXPECT_EQ(0, program.global_count);
}

/* §8.3 invalid references are dumped as '?' (never valid input). */
TEST(ir_dump_marks_invalid_references) {
    Ir_Program program;
    const Source_Span span = {0, 0, 1, 1};
    size_t block = 0;

    ir_program_init(&program);
    ASSERT_TRUE(ir_program_add_block(&program, "entry", 5, span, &block));

    const Ir_Instruction instructions[] = {
        ir_make_load(IR_NONE, 7, span),
        ir_make_store(7, 0, span),
        ir_make_br(0, 9, IR_NONE, span),
    };

    for (size_t i = 0; i < 3; ++i) {
        ir_block_append(&program.blocks[0], &instructions[i]);
    }

    char *text = dump_text(&program);
    EXPECT_STR_EQ("entry:\n"
                  "  %? = load @?\n"
                  "  store @?, %0\n"
                  "  br %0, ?, ?\n",
                  text);
    free(text);
    ir_program_free(&program);
}

/* ---- lowering --------------------------------------------------------- */

/* §8.5 globals are the symbols, with the same ids and folded names. */
TEST(lower_globals_match_symbols) {
    Ir_Program program;

    ASSERT_TRUE(lower_text("Beta = 1; ALPHA = beta; R0 = alpha;", &program));
    ASSERT_EQ(3, program.global_count);
    EXPECT_STR_EQ("beta", program.globals[0].name);
    EXPECT_STR_EQ("alpha", program.globals[1].name);
    EXPECT_STR_EQ("r0", program.globals[2].name);
    ir_program_free(&program);
}

/* §8.5, §4.9 the exact shape of '&&' and the program end. */
TEST(lower_logical_and_shape) {
    Ir_Program program;

    ASSERT_TRUE(lower_text("r0 = a && b;", &program));

    char *text = dump_text(&program);
    EXPECT_STR_EQ("global @r0\n"
                  "global @a\n"
                  "global @b\n"
                  "\n"
                  "entry:\n"
                  "  %0 = load @a\n"
                  "  %1 = const 0\n"
                  "  br %0, and.rhs.1, and.end.1\n"
                  "\n"
                  "and.rhs.1:\n"
                  "  %2 = load @b\n"
                  "  %3 = const 0\n"
                  "  %1 = ne %2, %3\n"
                  "  jmp and.end.1\n"
                  "\n"
                  "and.end.1:\n"
                  "  store @r0, %1\n"
                  "  %4 = load @r0\n"
                  "  exit %4\n",
                  text);
    free(text);
    ir_program_free(&program);
}

/* §8.5 '||' starts from 1 and skips the right operand when the left is
 * nonzero. */
TEST(lower_logical_or_shape) {
    Ir_Program program;

    ASSERT_TRUE(lower_text("x = a || b;", &program));
    ASSERT_EQ(3, program.block_count);

    const Ir_Block *entry = &program.blocks[0];
    const Ir_Instruction *result = &entry->instructions[entry->count - 2];
    const Ir_Instruction *branch = ir_block_terminator(entry);
    EXPECT_EQ(IR_CONST, result->opcode);
    EXPECT_EQ(1, result->value);
    ASSERT_TRUE(branch != NULL);
    EXPECT_EQ(IR_BR, branch->opcode);
    EXPECT_STR_EQ("or.end.1", program.blocks[branch->targets[0]].name);
    EXPECT_STR_EQ("or.rhs.1", program.blocks[branch->targets[1]].name);
    ir_program_free(&program);
}

/* §8.5 the layout follows the source; loops jump back to the condition,
 * break to the end, continue to the condition; dead blocks follow
 * break/continue. */
TEST(lower_loop_shapes) {
    static const char *const expected[] = {
        "entry", "while.cond.1", "while.body.1", "if.then.2", "dead.3",
        "if.end.2", "do.body.4", "dead.5", "do.cond.4", "do.end.4",
        "while.end.1",
    };
    Ir_Program program;

    ASSERT_TRUE(lower_text("while (x) { if (y) break; do continue; "
                           "while (z); }",
                           &program));
    ASSERT_EQ(sizeof(expected) / sizeof(expected[0]), program.block_count);

    for (size_t i = 0; i < program.block_count; ++i) {
        EXPECT_STR_EQ(expected[i], program.blocks[i].name);
    }

    /* if.then.2: jmp while.end.1 (break). */
    const Ir_Instruction *jump = ir_block_terminator(&program.blocks[3]);
    EXPECT_EQ(IR_JMP, jump->opcode);
    EXPECT_EQ(10, jump->targets[0]);
    /* do.body.4: jmp do.cond.4 (continue). */
    jump = ir_block_terminator(&program.blocks[6]);
    EXPECT_EQ(8, jump->targets[0]);
    /* do.cond.4: br %c, do.body.4, do.end.4. */
    jump = ir_block_terminator(&program.blocks[8]);
    EXPECT_EQ(IR_BR, jump->opcode);
    EXPECT_EQ(6, jump->targets[0]);
    EXPECT_EQ(9, jump->targets[1]);
    /* do.end.4 ends the while body: back to while.cond.1. */
    jump = ir_block_terminator(&program.blocks[9]);
    EXPECT_EQ(1, jump->targets[0]);

    Diagnostic error;
    EXPECT_TRUE(ir_verify(&program, &error));
    ir_program_free(&program);
}

/* §5.5 without r0 the program exits with the constant 0. */
TEST(lower_exit_without_r0) {
    Ir_Program program;

    ASSERT_TRUE(lower_text("", &program));
    ASSERT_EQ(1, program.block_count);
    ASSERT_EQ(2, program.blocks[0].count);
    EXPECT_EQ(IR_CONST, program.blocks[0].instructions[0].opcode);
    EXPECT_EQ(0, program.blocks[0].instructions[0].value);
    EXPECT_EQ(IR_EXIT, program.blocks[0].instructions[1].opcode);
    ir_program_free(&program);
}

/* ---- text format ------------------------------------------------------ */

static bool has_suffix(const char *name, const char *suffix) {
    const size_t length = strlen(name);
    const size_t suffix_length = strlen(suffix);
    return length >= suffix_length &&
           strcmp(name + length - suffix_length, suffix) == 0;
}

static char *read_whole_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return NULL;
    }

    char *buffer = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buffer, &size);
    int c;

    while (stream != NULL && (c = fgetc(file)) != EOF) {
        fputc(c, stream);
    }

    fclose(file);

    if (stream != NULL) {
        fclose(stream);
    }

    *length = size;
    return buffer;
}

/* §8.3, §8.4 every LIL program under tests/golden and examples that
 * passes the front end lowers to verified IR whose dump round-trips:
 * dump -> parse -> dump is the identity. Returns the number checked. */
static size_t round_trip_directory(const char *directory) {
    DIR *dir = opendir(directory);
    size_t checked = 0;

    if (dir == NULL) {
        return 0;
    }

    for (struct dirent *entry = readdir(dir); entry != NULL;
         entry = readdir(dir)) {
        if (!has_suffix(entry->d_name, ".lil")) {
            continue;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);

        size_t length = 0;
        char *text = read_whole_file(path, &length);
        Lil_Result result;

        if (text == NULL ||
            !lil_analyze_source(path, text, length, &result)) {
            free(text);
            lil_result_free(&result);
            continue;
        }

        Ir_Program program;
        Ir_Program again;
        Diagnostic error;

        EXPECT_TRUE(ir_lower_program(&result.program, &result.symbols,
                                     &program));
        EXPECT_TRUE(ir_verify(&program, &error));

        char *dumped = dump_text(&program);

        if (!parse_text(dumped, &again, &error)) {
            EXPECT_STR_EQ(path, error.message);
        } else {
            char *redumped = dump_text(&again);
            EXPECT_STR_EQ(dumped, redumped);
            EXPECT_EQ(program.vreg_count, again.vreg_count);
            free(redumped);
        }

        ++checked;
        free(dumped);
        ir_program_free(&again);
        ir_program_free(&program);
        lil_result_free(&result);
        free(text);
    }

    closedir(dir);
    return checked;
}

TEST(ir_round_trips_goldens_and_examples) {
    static const char *const directories[] = {
        "tests/golden/ast", "tests/golden/ir", "tests/golden/interp",
        "tests/golden/cli", "examples", "examples/accept",
    };
    size_t checked = 0;

    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]);
         ++i) {
        checked += round_trip_directory(directories[i]);
    }

    EXPECT_TRUE(checked >= 50);
}

/* §8.3 the parser: spacing, comments, CRLF, extreme integers, forward
 * labels, vreg_count, unresolved names left to the verifier. */
TEST(ir_parse_reads_the_text_format) {
    Ir_Program program;
    Diagnostic error;

    ASSERT_TRUE(parse_text("; header\r\n"
                           "global @g ; comment\n"
                           "\n"
                           "a:\n"
                           "\t%7 =const -9223372036854775808\r\n"
                           "  %2 = const 9223372036854775807\n"
                           "  %3 = const -0\n"
                           "  store @nope, %2\n"
                           "  br %7 , b , missing\n"
                           "b: ; trailing\n"
                           "  exit %3",
                           &program, &error));
    EXPECT_EQ(1, program.global_count);
    ASSERT_EQ(2, program.block_count);
    EXPECT_EQ(8, program.vreg_count);
    ASSERT_EQ(5, program.blocks[0].count);
    EXPECT_EQ(INT64_MIN, program.blocks[0].instructions[0].value);
    EXPECT_EQ(5, program.blocks[0].instructions[0].span.line);
    EXPECT_EQ(2, program.blocks[0].instructions[0].span.column);
    EXPECT_EQ(INT64_MAX, program.blocks[0].instructions[1].value);
    EXPECT_EQ(0, program.blocks[0].instructions[2].value);
    EXPECT_EQ(IR_NONE, program.blocks[0].instructions[3].global);
    EXPECT_EQ(1, program.blocks[0].instructions[4].targets[0]);
    EXPECT_EQ(IR_NONE, program.blocks[0].instructions[4].targets[1]);
    EXPECT_EQ(4, program.blocks[0].span.line);
    ir_program_free(&program);

    /* Many labels and globals (the name maps grow). */
    char text[8192];
    size_t used = 0;

    for (int i = 0; i < 40; ++i) {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "global @g%d\n", i);
    }

    for (int i = 0; i < 40; ++i) {
        used += (size_t)snprintf(text + used, sizeof(text) - used,
                                 "l%d:\n  jmp l%d\n", i, (i + 1) % 40);
    }

    ASSERT_TRUE(parse_text(text, &program, &error));
    EXPECT_EQ(40, program.global_count);
    EXPECT_EQ(40, program.block_count);
    EXPECT_EQ(0, program.blocks[39].instructions[0].targets[0]);
    EXPECT_TRUE(ir_verify(&program, &error));
    ir_program_free(&program);
}

/* §8.3 every parse error, with its position. */
TEST(ir_parse_reports_errors_with_positions) {
    expect_parse_error("a:\n  %0 = const 1 #", 2, 16,
                       "unexpected character '#'");
    expect_parse_error("a:\n  \x7f", 2, 3, "unexpected character '\\x7f'");
    expect_parse_error("global @ x", 1, 8, "expected a name after '@'");
    expect_parse_error("a:\n  %", 2, 3, "expected a number after '%'");
    expect_parse_error("a:\n  %99999999999999999999999 = const 1", 2, 3,
                       "virtual register number too large (the limit is "
                       "16777215)");
    expect_parse_error("a:\n  %0 = const -x", 2, 14,
                       "expected a digit after '-'");
    expect_parse_error("a:\n  %0 = const 9223372036854775808", 2, 14,
                       "integer out of range");
    expect_parse_error("a:\n  %0 = const 1\n  %1 = add %0 %0", 3, 15,
                       "expected ','");
    expect_parse_error("a:\n  putc 1", 2, 8, "expected a virtual register");
    expect_parse_error("a:\n  %0 = load", 2, 12,
                       "expected a global (@name)");
    expect_parse_error("a:\n  jmp", 2, 6, "expected a block name");
    expect_parse_error("a:\n  %0 = const", 2, 13, "expected an integer");
    expect_parse_error("a:\n  %0 := const 1", 2, 6, "expected '='");
    expect_parse_error("a:\n  %0 = %1", 2, 8,
                       "expected an instruction name");
    expect_parse_error("a:\n  %0 = mod %1", 2, 8,
                       "unknown instruction 'mod'");
    expect_parse_error("a:\n  neg %1", 2, 3,
                       "'neg' needs a destination register "
                       "('%N = neg ...')");
    expect_parse_error("a:\n  %0 = jmp a", 2, 8,
                       "'jmp' does not produce a value");
    expect_parse_error("exit %0", 1, 1, "instruction outside of a block");
    expect_parse_error("a:\n  jmp a\nglobal @g", 3, 1,
                       "global declaration after the first block");
    expect_parse_error("global @g\nglobal @g", 2, 8,
                       "duplicate global '@g'");
    expect_parse_error("a:\n  jmp a\na:", 3, 1, "duplicate block 'a'");
    expect_parse_error("=", 1, 1,
                       "expected an instruction, a label or a global "
                       "declaration");
    expect_parse_error("a: b:", 1, 4, "expected end of line");

    /* Long names are shortened in messages (200 bytes and "..."). */
    char text[1300];
    char expected[300];

    memset(text, 'n', 600);
    text[600] = ':';
    text[601] = '\n';
    memcpy(text + 602, text, 602);
    text[1204] = '\0';
    snprintf(expected, sizeof(expected), "duplicate block '%.200s...'",
             text);
    expect_parse_error(text, 2, 1, expected);
}

/* ---- verifier --------------------------------------------------------- */

/* §8.4 one negative case per rule, from IR text. */
TEST(ir_verify_rejects_each_rule_from_text) {
    /* Rule 1. */
    expect_verify_error("global @x\n", 1, 1, "program has no blocks");
    /* Rule 2. */
    expect_verify_error("a:\n  %0 = load @x\n  exit %0", 2, 3,
                        "'load' refers to a global that does not exist");
    expect_verify_error("a:\n  %0 = const 0\n  store @x, %0\n  exit %0", 3,
                        3, "'store' refers to a global that does not exist");
    expect_verify_error("a:\n  jmp b", 2, 3,
                        "'jmp' jumps to a block that does not exist");
    /* Rule 3. */
    expect_verify_error("a:\n  %0 = const 1", 1, 1,
                        "block 'a' does not end with a terminator");
    expect_verify_error("a:\n  jmp b\nb:", 3, 1,
                        "block 'b' does not end with a terminator");
    expect_verify_error("a:\n  jmp a\n  jmp a", 2, 3,
                        "terminator 'jmp' is not the last instruction of "
                        "block 'a'");
    /* Rule 4. */
    expect_verify_error("a:\n  exit %0", 2, 3,
                        "virtual register %0 may be used before it is "
                        "defined");
    expect_verify_error("a:\n  %0 = const 1\n  br %0, b, c\n"
                        "b:\n  %1 = const 1\n  jmp c\n"
                        "c:\n  exit %1",
                        8, 3,
                        "virtual register %1 may be used before it is "
                        "defined");
    expect_verify_error("a:\n  jmp b\nb:\n  putc %0\n  %0 = const 1\n"
                        "  jmp b",
                        4, 3,
                        "virtual register %0 may be used before it is "
                        "defined");
    /* Undefined on a path two blocks back. */
    expect_verify_error("a:\n  %0 = const 1\n  br %0, b, c\n"
                        "b:\n  %1 = const 1\n  jmp d\n"
                        "c:\n  jmp d\n"
                        "d:\n  jmp e\n"
                        "e:\n  exit %1",
                        12, 3,
                        "virtual register %1 may be used before it is "
                        "defined");
    /* Defined only inside the loop, used after it: the loop may run zero
     * times. */
    expect_verify_error("a:\n  %0 = const 0\n  jmp h\n"
                        "h:\n  br %0, body, end\n"
                        "body:\n  %1 = const 1\n  jmp h\n"
                        "end:\n  exit %1",
                        10, 3,
                        "virtual register %1 may be used before it is "
                        "defined");
    /* An unreachable predecessor that defines the vreg does not help. */
    expect_verify_error("a:\n  jmp d\n"
                        "o:\n  %1 = const 1\n  jmp d\n"
                        "d:\n  exit %1",
                        7, 3,
                        "virtual register %1 may be used before it is "
                        "defined");
    /* The first violation in program order is reported. */
    expect_verify_error("a:\n  %0 = const 1\n  br %0, b, c\n"
                        "b:\n  putc %2\n  jmp c\n"
                        "c:\n  putc %1\n  exit %2",
                        5, 3,
                        "virtual register %2 may be used before it is "
                        "defined");
    expect_verify_error("a:\n  %0 = const 1\n  br %0, b, c\n"
                        "b:\n  putc %0\n  putc %1\n  putc %2\n  jmp c\n"
                        "c:\n  exit %0",
                        6, 3,
                        "virtual register %1 may be used before it is "
                        "defined");
    expect_verify_error("a:\n  %0 = const 1\n  %1 = add %0, %3\n  exit %1",
                        3, 3,
                        "virtual register %3 may be used before it is "
                        "defined");
    /* The entry has nothing defined on entry even when it has
     * predecessors. */
    expect_verify_error("a:\n  putc %0\n  %0 = const 1\n  jmp a", 2, 3,
                        "virtual register %0 may be used before it is "
                        "defined");
}

/* §8.4 accepted programs: definitions on every path, through loops and
 * diamonds; unreachable blocks are not checked. */
TEST(ir_verify_accepts_definitions_on_every_path) {
    expect_valid("a:\n  %0 = const 1\n  br %0, b, c\n"
                 "b:\n  %1 = const 1\n  jmp d\n"
                 "c:\n  %1 = const 2\n  jmp d\n"
                 "d:\n  exit %1");
    /* Defined before the loop, used in it; redefined in the latch. */
    expect_valid("a:\n  %0 = const 1\n  jmp b\n"
                 "b:\n  putc %0\n  %0 = add %0, %0\n  br %0, b, c\n"
                 "c:\n  exit %0");
    /* Found two blocks back, through blocks that do not define it. */
    expect_valid("a:\n  %0 = const 1\n  jmp b\n"
                 "b:\n  jmp c\n"
                 "c:\n  br %0, b, d\n"
                 "d:\n  exit %0");
    /* Defined in the block, then used twice. */
    expect_valid("a:\n  %0 = const 1\n  putc %0\n  exit %0");
    /* Unreachable cycle. */
    expect_valid("a:\n  %0 = const 1\n  exit %0\n"
                 "x:\n  putc %5\n  jmp y\n"
                 "y:\n  %5 = const 1\n  jmp x");
    /* Unreachable blocks are not checked, even without predecessors. */
    expect_valid("a:\n  %0 = const 1\n  exit %0\n"
                 "orphan:\n  putc %7\n  exit %0");
    /* An unreachable predecessor that does not define the vreg does not
     * hurt. */
    expect_valid("a:\n  %1 = const 1\n  jmp d\n"
                 "o:\n  jmp d\n"
                 "d:\n  exit %1");
    /* Defined in the entry: defined everywhere after it, also through a
     * loop back to the entry. */
    expect_valid("a:\n  %0 = const 1\n  br %0, b, c\n"
                 "b:\n  putc %0\n  jmp a\n"
                 "c:\n  exit %0");
    /* Defined on both sides of a diamond inside a loop. */
    expect_valid("a:\n  %0 = const 1\n  jmp h\n"
                 "h:\n  br %0, l, r\n"
                 "l:\n  %1 = const 1\n  jmp j\n"
                 "r:\n  %1 = const 2\n  jmp j\n"
                 "j:\n  putc %1\n  br %1, h, e\n"
                 "e:\n  exit %1");
    /* A self-loop that redefines what it uses, entered with it defined. */
    expect_valid("a:\n  jmp s\n"
                 "s:\n  %0 = const 3\n  jmp l\n"
                 "l:\n  %0 = sub %0, %0\n  br %0, l, e\n"
                 "e:\n  exit %0");
}

/* §8.4 rule 4, §8.7: a pass that folds a constant 'br' into a 'jmp'
 * leaves the other target without predecessors; the result is still
 * valid IR (the orphan is unreachable and is not checked). */
TEST(ir_verify_accepts_a_folded_branch_orphan) {
    Ir_Program program;
    Diagnostic error;

    ASSERT_TRUE(lower_text("r0 = 7 + (0 && b);", &program));
    EXPECT_TRUE(ir_verify(&program, &error));

    const size_t end = ir_program_find_block(&program, "and.end.1", 9);
    ASSERT_TRUE(end != IR_NONE);
    ASSERT_TRUE(ir_program_find_block(&program, "and.rhs.1", 9) != IR_NONE);

    Ir_Block *entry = &program.blocks[0];
    Ir_Instruction *terminator = &entry->instructions[entry->count - 1];
    ASSERT_TRUE(terminator->opcode == IR_BR);
    *terminator = ir_make_jmp(end, terminator->span);

    EXPECT_TRUE(ir_verify(&program, &error));
    EXPECT_STR_EQ("", error.message);

    Ir_Interp_Result result;
    char *output = NULL;
    run_program(&program, 1000, &result, &output);
    EXPECT_EQ(IR_INTERP_EXITED, result.status);
    EXPECT_EQ(7, result.exit_code);
    free(output);
    ir_program_free(&program);
}

/* Appends block 'name' to 'program'; returns its index. */
static size_t add_named_block(Ir_Program *program, const char *prefix,
                              size_t number) {
    char name[64];
    size_t index = 0;
    const int length = snprintf(name, sizeof(name), "%s%zu", prefix, number);

    ir_program_add_block(program, name, (size_t)length,
                         (Source_Span){0, 0, number + 1, 1}, &index);
    return index;
}

static double seconds_since(clock_t start) {
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

/* §8.4 rule 4 is near-linear: a chain of 200000 blocks and a ladder of
 * 100000 diamonds, each using a vreg defined in the entry or in a later
 * block (so the dataflow runs), verify well within 2 seconds. */
TEST(ir_verify_is_fast_on_long_chains_and_ladders) {
    const Source_Span span = {0, 0, 1, 1};

    for (int defined_in_entry = 0; defined_in_entry < 2; ++defined_in_entry) {
        Ir_Program program;
        Diagnostic error;
        const size_t blocks = 200000;

        ir_program_init(&program);
        program.vreg_count = 1;
        add_named_block(&program, "entry", 0);

        for (size_t i = 1; i <= blocks; ++i) {
            add_named_block(&program, "b", i);
        }

        /* entry: [%0 = const 1]; jmp b1. b1: [%0 = const 1]; putc %0;
         * jmp b2. ... last: putc %0; exit %0. */
        for (size_t i = 0; i <= blocks; ++i) {
            Ir_Block *block = &program.blocks[i];
            Ir_Instruction instruction = ir_make_const(0, 1, span);

            if (i == (defined_in_entry ? 0u : 1u)) {
                ir_block_append(block, &instruction);
            }

            if (i > 0) {
                instruction = ir_make_putc(0, span);
                ir_block_append(block, &instruction);
            }

            instruction = i == blocks ? ir_make_exit(0, span)
                                      : ir_make_jmp(i + 1, span);
            ir_block_append(block, &instruction);
        }

        const clock_t start = clock();
        EXPECT_TRUE(ir_verify(&program, &error));
        const double elapsed = seconds_since(start);
        EXPECT_TRUE(elapsed < 2.0);

        if (elapsed >= 2.0) {
            printf("  chain verified in %.2f s\n", elapsed);
        }

        ir_program_free(&program);
    }

    for (int defined_in_entry = 0; defined_in_entry < 2; ++defined_in_entry) {
        Ir_Program program;
        Diagnostic error;
        const size_t diamonds = 100000;
        Ir_Instruction instruction;

        /* entry, s, then d_i, l_i, r_i per diamond, then d_N. */
        ir_program_init(&program);
        program.vreg_count = 1;
        add_named_block(&program, "entry", 0);
        add_named_block(&program, "s", 0);

        for (size_t i = 0; i < diamonds; ++i) {
            add_named_block(&program, "d", i);
            add_named_block(&program, "l", i);
            add_named_block(&program, "r", i);
        }

        add_named_block(&program, "d", diamonds);

        instruction = ir_make_const(0, 0, span);
        ir_block_append(&program.blocks[defined_in_entry ? 0 : 1],
                        &instruction);
        instruction = ir_make_jmp(1, span);
        ir_block_append(&program.blocks[0], &instruction);
        instruction = ir_make_jmp(2, span);
        ir_block_append(&program.blocks[1], &instruction);

        for (size_t i = 0; i < diamonds; ++i) {
            const size_t d = 2 + 3 * i;

            instruction = ir_make_br(0, d + 1, d + 2, span);
            ir_block_append(&program.blocks[d], &instruction);
            instruction = ir_make_jmp(d + 3, span);
            ir_block_append(&program.blocks[d + 1], &instruction);
            instruction = ir_make_putc(0, span);
            ir_block_append(&program.blocks[d + 2], &instruction);
            instruction = ir_make_jmp(d + 3, span);
            ir_block_append(&program.blocks[d + 2], &instruction);
        }

        instruction = ir_make_exit(0, span);
        ir_block_append(&program.blocks[program.block_count - 1],
                        &instruction);

        const clock_t start = clock();
        EXPECT_TRUE(ir_verify(&program, &error));
        const double elapsed = seconds_since(start);
        EXPECT_TRUE(elapsed < 2.0);

        if (elapsed >= 2.0) {
            printf("  ladder verified in %.2f s\n", elapsed);
        }

        ir_program_free(&program);
    }
}

/*
 * §8.4 rule 4 stays near-linear with one cross-block vreg per diamond
 * (each diamond's condition is used again in the next diamond's head),
 * which gives as many tracked vregs as diamonds, and on the lowering of
 * 200000 lines of 'x = a && b;' at -O0 (200000 tracked vregs across
 * 400000 blocks). The bitset dataflow alone took 4 and 19 s here.
 */
TEST(ir_verify_is_fast_with_a_vreg_per_diamond) {
    const Source_Span span = {0, 0, 1, 1};
    static const size_t sizes[] = {100000, 200000};

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        const size_t diamonds = sizes[s];
        Ir_Program program;
        Diagnostic error;
        Ir_Instruction instruction;

        /* d_i: [putc %(i-1)]; %i = const 1; br %i, l_i, r_i.
         * l_i: putc %i; jmp d_(i+1). r_i: jmp d_(i+1). d_N: exit. */
        ir_program_init(&program);
        program.vreg_count = diamonds;

        for (size_t i = 0; i < diamonds; ++i) {
            add_named_block(&program, i == 0 ? "entry" : "d", i);
            add_named_block(&program, "l", i);
            add_named_block(&program, "r", i);
        }

        const size_t last = add_named_block(&program, "d", diamonds);

        for (size_t i = 0; i < diamonds; ++i) {
            const size_t d = 3 * i;

            if (i > 0) {
                instruction = ir_make_putc(i - 1, span);
                ir_block_append(&program.blocks[d], &instruction);
            }

            instruction = ir_make_const(i, 1, span);
            ir_block_append(&program.blocks[d], &instruction);
            instruction = ir_make_br(i, d + 1, d + 2, span);
            ir_block_append(&program.blocks[d], &instruction);
            instruction = ir_make_putc(i, span);
            ir_block_append(&program.blocks[d + 1], &instruction);
            instruction = ir_make_jmp(d + 3, span);
            ir_block_append(&program.blocks[d + 1], &instruction);
            ir_block_append(&program.blocks[d + 2], &instruction);
        }

        instruction = ir_make_exit(diamonds - 1, span);
        ir_block_append(&program.blocks[last], &instruction);

        const clock_t start = clock();
        EXPECT_TRUE(ir_verify(&program, &error));
        const double elapsed = seconds_since(start);
        /* About 0.4 and 0.8 s unoptimized; generous for loaded
         * machines and sanitizers, still far below the quadratic 17 s. */
        EXPECT_TRUE(elapsed < 5.0);

        if (elapsed >= 5.0) {
            printf("  %zu diamonds verified in %.2f s\n", diamonds, elapsed);
        }

        ir_program_free(&program);
    }

    const size_t lines = 200000;
    const char line[] = "x = a && b;\n";
    char *text = malloc(lines * (sizeof(line) - 1) + 1);
    Ir_Program program;
    Diagnostic error;

    ASSERT_TRUE(text != NULL);

    for (size_t i = 0; i < lines; ++i) {
        memcpy(text + i * (sizeof(line) - 1), line, sizeof(line) - 1);
    }

    text[lines * (sizeof(line) - 1)] = '\0';

    if (lower_text(text, &program)) {
        EXPECT_EQ(2 * lines + 1, program.block_count);

        const clock_t start = clock();
        EXPECT_TRUE(ir_verify(&program, &error));
        const double elapsed = seconds_since(start);
        EXPECT_TRUE(elapsed < 5.0);

        if (elapsed >= 5.0) {
            printf("  'x = a && b;' x %zu verified in %.2f s\n", lines,
                   elapsed);
        }
    } else {
        EXPECT_TRUE(false);
    }

    ir_program_free(&program);
    free(text);
}

/*
 * §8.4 rule 4 with more tracked vregs than one bitset slice holds: block
 * b_i uses %i, every %i is defined in block s. The entry can also jump
 * straight to the last chain block, so its use (of the last-numbered
 * vreg, in the last slice) fails, and so does a use of %0 (first slice)
 * in the block after it; the earlier one in program order is reported.
 */
TEST(ir_verify_handles_many_tracked_vregs) {
    const Source_Span span = {0, 0, 1, 1};
    /* 50 stays within the backward search's budget; 30000 vregs, each
     * used i blocks after its definition, exceed it and take the bitset
     * dataflow, in several slices. Both report the same use. */
    static const size_t counts[] = {50, 30000};

    for (int run = 0; run < 4; ++run) {
        const size_t count = counts[run / 2];
        const int broken = run % 2;
        Ir_Program program;
        Diagnostic error;
        Ir_Instruction instruction;

        ir_program_init(&program);
        program.vreg_count = count;
        add_named_block(&program, "entry", 0);
        add_named_block(&program, "s", 0);

        for (size_t i = 0; i < count; ++i) {
            add_named_block(&program, "b", i);
        }

        const size_t fin = add_named_block(&program, "fin", 0);

        if (broken) {
            /* entry: %count = const 1; br %count, s, b_{count-1}. */
            program.vreg_count = count + 1;
            instruction = ir_make_const(count, 1, span);
            ir_block_append(&program.blocks[0], &instruction);
            instruction = ir_make_br(count, 1, fin - 1, span);
        } else {
            instruction = ir_make_jmp(1, span);
        }

        ir_block_append(&program.blocks[0], &instruction);

        for (size_t i = 0; i < count; ++i) {
            instruction = ir_make_const(i, (int64_t)i, span);
            ir_block_append(&program.blocks[1], &instruction);
        }

        instruction = ir_make_jmp(2, span);
        ir_block_append(&program.blocks[1], &instruction);

        for (size_t i = 0; i < count; ++i) {
            Ir_Block *block = &program.blocks[2 + i];

            instruction = ir_make_putc(i, (Source_Span){0, 0, 2 + i, 3});
            ir_block_append(block, &instruction);
            instruction = ir_make_jmp(3 + i, span);
            ir_block_append(block, &instruction);
        }

        instruction = ir_make_putc(0, (Source_Span){0, 0, 999999, 3});
        ir_block_append(&program.blocks[fin], &instruction);
        instruction = ir_make_exit(0, span);
        ir_block_append(&program.blocks[fin], &instruction);

        if (broken) {
            EXPECT_FALSE(ir_verify(&program, &error));
            EXPECT_EQ(count + 1, error.span.line);
            char expected[80];
            snprintf(expected, sizeof(expected),
                     "virtual register %%%zu may be used before it is "
                     "defined",
                     count - 1);
            EXPECT_STR_EQ(expected, error.message);
        } else {
            EXPECT_TRUE(ir_verify(&program, &error));
        }

        ir_program_free(&program);
    }
}

/* §8.4 rules only reachable through the API (the parser rejects or
 * cannot express them). */
TEST(ir_verify_rejects_malformed_api_programs) {
    const Source_Span span = {0, 0, 3, 4};
    Ir_Program program;
    Diagnostic error;
    size_t block = 0;

    /* Duplicate block names. */
    ir_program_init(&program);
    ir_program_add_block(&program, "a", 1, span, &block);
    ir_program_add_block(&program, "b", 1, span, &block);
    ir_program_add_block(&program, "a", 1, (Source_Span){5, 0, 7, 1},
                         &block);
    EXPECT_FALSE(ir_verify(&program, &error));
    EXPECT_STR_EQ("duplicate block 'a'", error.message);
    EXPECT_EQ(7, error.span.line);
    ir_program_free(&program);

    /* Invalid opcode; missing and out-of-range vregs. */
    Ir_Instruction bad = ir_make_exit(0, span);
    bad.opcode = (Ir_Opcode)42;
    const Ir_Instruction cases[] = {
        bad,
        ir_make_exit(IR_NONE, span),
        ir_make_binary(IR_ADD, 0, 0, 3, span),
        ir_make_const(5, 1, span),
    };
    const char *const messages[] = {
        "invalid opcode 42",
        "'exit' is missing a virtual register",
        "virtual register %3 is out of range (the program has 1)",
        "virtual register %5 is out of range (the program has 1)",
    };

    for (size_t i = 0; i < 4; ++i) {
        ir_program_init(&program);
        ir_program_add_block(&program, "a", 1, span, &block);
        program.vreg_count = 1;
        ir_block_append(&program.blocks[0], &cases[i]);
        EXPECT_FALSE(ir_verify(&program, &error));
        EXPECT_STR_EQ(messages[i], error.message);
        EXPECT_EQ(3, error.span.line);
        ir_program_free(&program);
    }
}

/* ---- interpreter ------------------------------------------------------ */

/* §4.5-§4.8, §8.2 the arithmetic, including wraparound and traps. */
TEST(ir_eval_follows_the_spec) {
    int64_t r = 0;

    EXPECT_TRUE(ir_eval_binary(IR_ADD, INT64_MAX, 1, &r));
    EXPECT_EQ(INT64_MIN, r);
    EXPECT_TRUE(ir_eval_binary(IR_SUB, INT64_MIN, 1, &r));
    EXPECT_EQ(INT64_MAX, r);
    EXPECT_TRUE(ir_eval_binary(IR_MUL, INT64_MAX, 2, &r));
    EXPECT_EQ(-2, r);
    EXPECT_TRUE(ir_eval_binary(IR_MUL, INT64_MIN, -1, &r));
    EXPECT_EQ(INT64_MIN, r);
    EXPECT_TRUE(ir_eval_binary(IR_DIV, -7, 2, &r));
    EXPECT_EQ(-3, r);
    EXPECT_TRUE(ir_eval_binary(IR_DIV, 7, -2, &r));
    EXPECT_EQ(-3, r);
    EXPECT_TRUE(ir_eval_binary(IR_DIV, INT64_MIN, 1, &r));
    EXPECT_EQ(INT64_MIN, r);
    EXPECT_TRUE(ir_eval_binary(IR_AND, 12, 10, &r));
    EXPECT_EQ(8, r);
    EXPECT_TRUE(ir_eval_binary(IR_OR, -8, 3, &r));
    EXPECT_EQ(-5, r);
    EXPECT_TRUE(ir_eval_binary(IR_LT, INT64_MIN, 0, &r));
    EXPECT_EQ(1, r);
    EXPECT_TRUE(ir_eval_binary(IR_LE, 1, 0, &r));
    EXPECT_EQ(0, r);
    EXPECT_TRUE(ir_eval_binary(IR_GT, 0, -1, &r));
    EXPECT_EQ(1, r);
    EXPECT_TRUE(ir_eval_binary(IR_GE, -1, 0, &r));
    EXPECT_EQ(0, r);
    EXPECT_TRUE(ir_eval_binary(IR_EQ, 3, 3, &r));
    EXPECT_EQ(1, r);
    EXPECT_TRUE(ir_eval_binary(IR_NE, 3, 3, &r));
    EXPECT_EQ(0, r);

    r = 99;
    EXPECT_FALSE(ir_eval_binary(IR_DIV, 1, 0, &r));
    EXPECT_FALSE(ir_eval_binary(IR_DIV, 0, 0, &r));
    EXPECT_FALSE(ir_eval_binary(IR_DIV, INT64_MIN, -1, &r));
    EXPECT_EQ(99, r);

    EXPECT_EQ(INT64_MIN, ir_eval_unary(IR_NEG, INT64_MIN));
    EXPECT_EQ(-INT64_MAX, ir_eval_unary(IR_NEG, INT64_MAX));
    EXPECT_EQ(-1, ir_eval_unary(IR_NOT, 0));
    EXPECT_EQ(INT64_MIN, ir_eval_unary(IR_NOT, INT64_MAX));
    EXPECT_EQ(42, ir_eval_unary(IR_COPY, 42));
}

/* §4.5 INT64_MAX + 1 in a whole program; §5.5 exit status r0 & 0xff. */
TEST(interp_wraps_and_masks_the_exit_status) {
    expect_run("r0 = 9223372036854775807 + 1 == -9223372036854775807 - 1;",
               "", IR_INTERP_EXITED, 1);
    expect_run("r0 = -1;", "", IR_INTERP_EXITED, 255);
    expect_run("r0 = 256;", "", IR_INTERP_EXITED, 0);
    expect_run("x = 5;", "", IR_INTERP_EXITED, 0);
}

/* §4.9 the right operand runs only when needed; §4.10 putc's value. */
TEST(interp_short_circuits_and_putc_returns_its_argument) {
    expect_run("a = 0 && putc('A'); b = 2 && putc('B');"
               "c = 3 || putc('C'); d = 0 || putc('D');"
               "r0 = a * 1000 + b * 100 + c * 10 + d;",
               "BD", IR_INTERP_EXITED, 111);
    expect_run("r0 = putc(256 + 'z') - 256;", "z", IR_INTERP_EXITED, 'z');
    expect_run("putc(-191);", "A", IR_INTERP_EXITED, 0);
    expect_run("x = 0 && (1 / 0); y = 1 || (1 / 0); r0 = x + y;", "",
               IR_INTERP_EXITED, 1);
}

/* §4.6 traps stop the program at the div; output so far is kept. */
TEST(interp_reports_traps) {
    expect_run("putc('a'); r0 = 1 / x; putc('b');", "a", IR_INTERP_TRAPPED,
               0);
    expect_run("m = -9223372036854775807 - 1; r0 = m / -1;", "",
               IR_INTERP_TRAPPED, 0);
    expect_run("x / 0; putc('n');", "", IR_INTERP_TRAPPED, 0);

    Ir_Program program;
    ASSERT_TRUE(lower_text("x = 1;\nr0 = (3 / (x - 1));", &program));

    Ir_Interp_Result result;
    char *output = NULL;
    run_program(&program, 0, &result, &output);
    EXPECT_EQ(IR_INTERP_TRAPPED, result.status);
    EXPECT_EQ(2, result.trap_span.line);
    EXPECT_EQ(6, result.trap_span.column);
    free(output);
    ir_program_free(&program);
}

/* §8.6 the step limit stops an endless loop after exactly N steps; a
 * program that needs exactly N steps still finishes. */
TEST(interp_step_limit) {
    Ir_Program program;
    Ir_Interp_Result result;
    char *output = NULL;

    ASSERT_TRUE(lower_text("putc('s'); while (1) ;", &program));
    run_program(&program, 1000, &result, &output);
    EXPECT_EQ(IR_INTERP_STEP_LIMIT, result.status);
    EXPECT_EQ(1000, result.steps);
    EXPECT_STR_EQ("s", output);
    free(output);
    ir_program_free(&program);

    ASSERT_TRUE(lower_text("putc('k');", &program));
    run_program(&program, 4, &result, &output);
    EXPECT_EQ(IR_INTERP_EXITED, result.status);
    EXPECT_EQ(4, result.steps);
    free(output);
    run_program(&program, 3, &result, &output);
    EXPECT_EQ(IR_INTERP_STEP_LIMIT, result.status);
    free(output);
    ir_program_free(&program);
}

/* Runs ir_interp_terminate() for 'text' in a child; returns its wait
 * status. */
static int terminate_in_child(const char *text) {
    fflush(stdout);
    fflush(stderr);

    const pid_t pid = fork();

    if (pid == 0) {
        Ir_Program program;
        Ir_Interp_Result result;

        if (!lower_text(text, &program)) {
            _exit(99);
        }

        FILE *sink = fopen("/dev/null", "w");
        ir_interp_run(&program, sink, 0, &result);
        fclose(sink);
        ir_program_free(&program);
        ir_interp_terminate(&result);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

/* §5.6 a trap ends the process by SIGFPE (136 in the shell), even when
 * SIGFPE was ignored or blocked; otherwise the status is r0 & 0xff. */
TEST(interp_terminate_matches_native_programs) {
    int status = terminate_in_child("r0 = 1 / 0;");
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(SIGFPE, WIFSIGNALED(status) ? WTERMSIG(status) : 0);

    struct sigaction ignore;
    struct sigaction saved;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGFPE, &ignore, &saved);
    sigset_t block;
    sigset_t saved_mask;
    sigemptyset(&block);
    sigaddset(&block, SIGFPE);
    sigprocmask(SIG_BLOCK, &block, &saved_mask);

    status = terminate_in_child("m = -9223372036854775807 - 1; r0 = m / -1;");
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(SIGFPE, WIFSIGNALED(status) ? WTERMSIG(status) : 0);

    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    sigaction(SIGFPE, &saved, NULL);

    status = terminate_in_child("r0 = 300;");
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(44, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/* §5.6 ir_interp_terminate() first undoes an ignored or blocked SIGFPE. */
TEST(interp_restores_the_default_sigfpe_action) {
    struct sigaction ignore;
    struct sigaction saved;
    struct sigaction current;
    sigset_t block;
    sigset_t saved_mask;
    sigset_t mask;

    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGFPE, &ignore, &saved);
    sigemptyset(&block);
    sigaddset(&block, SIGFPE);
    sigprocmask(SIG_BLOCK, &block, &saved_mask);

    ir_interp_restore_sigfpe();

    sigaction(SIGFPE, NULL, &current);
    sigprocmask(SIG_BLOCK, NULL, &mask);
    EXPECT_TRUE(current.sa_handler == SIG_DFL);
    EXPECT_FALSE(sigismember(&mask, SIGFPE));

    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    sigaction(SIGFPE, &saved, NULL);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        test_set_filter(argv[1]);
    }

    RUN_TEST(ir_opcode_table_is_consistent);
    RUN_TEST(ir_program_api_builds_and_frees);
    RUN_TEST(ir_dump_marks_invalid_references);
    RUN_TEST(lower_globals_match_symbols);
    RUN_TEST(lower_logical_and_shape);
    RUN_TEST(lower_logical_or_shape);
    RUN_TEST(lower_loop_shapes);
    RUN_TEST(lower_exit_without_r0);
    RUN_TEST(ir_round_trips_goldens_and_examples);
    RUN_TEST(ir_parse_reads_the_text_format);
    RUN_TEST(ir_parse_reports_errors_with_positions);
    RUN_TEST(ir_verify_rejects_each_rule_from_text);
    RUN_TEST(ir_verify_accepts_definitions_on_every_path);
    RUN_TEST(ir_verify_rejects_malformed_api_programs);
    RUN_TEST(ir_verify_accepts_a_folded_branch_orphan);
    RUN_TEST(ir_verify_is_fast_on_long_chains_and_ladders);
    RUN_TEST(ir_verify_is_fast_with_a_vreg_per_diamond);
    RUN_TEST(ir_verify_handles_many_tracked_vregs);
    RUN_TEST(ir_eval_follows_the_spec);
    RUN_TEST(interp_wraps_and_masks_the_exit_status);
    RUN_TEST(interp_short_circuits_and_putc_returns_its_argument);
    RUN_TEST(interp_reports_traps);
    RUN_TEST(interp_step_limit);
    RUN_TEST(interp_terminate_matches_native_programs);
    RUN_TEST(interp_restores_the_default_sigfpe_action);
    return test_finish();
}
