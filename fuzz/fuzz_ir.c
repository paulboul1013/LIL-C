#define _POSIX_C_SOURCE 200809L

/*
 * libFuzzer harness for the IR text path (--ir-input, docs/spec.md §8):
 * arbitrary bytes -> ir_parse() -> ir_verify() -> dump / parse / dump
 * identity -> interpreter with a small step limit -> ir_optimize() ->
 * verifier -> interpreter again, compared with the first run.
 *
 * Contract: for any input the parser either builds a program or reports
 * an 'ir' diagnostic, and the verifier either accepts it or reports a
 * 'verify' diagnostic -- never a crash, out-of-bounds access, undefined
 * behaviour, leak or hang (the verifier is near-linear, §8.4). A program
 * that verifies must dump in canonical form, parse back to the same dump
 * (§8.3), and run identically before and after the round trip (§8.6).
 * After -O1 (§8.7) it must still verify, behave like the unoptimized
 * program on the interpreter (same output and status, never more steps;
 * fuzz_opt.h) and be a fixpoint of -O1.
 *
 * Build and run with 'make fuzz' / 'make fuzz-run FUZZ_TARGET=ir'.
 */
#include "diagnostic.h"
#include "ir.h"
#include "ir_interp.h"
#include "ir_opt.h"
#include "ir_parse.h"
#include "ir_verify.h"
#include "source.h"

#include "fuzz_opt.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Small enough that a loop in the input cannot slow the fuzzer down. */
#define FUZZ_INTERP_STEPS 2000

static void fuzz_check(int condition) {
    if (!condition) {
        abort();
    }
}

static FILE *fuzz_sink(void) {
    static FILE *sink;

    if (sink == NULL) {
        sink = fopen("/dev/null", "w");
    }

    return sink;
}

/* The §8.3 dump as a malloc'ed buffer (NULL when out of memory). */
static char *dump_program(const Ir_Program *program, size_t *length) {
    char *buffer = NULL;
    FILE *stream = open_memstream(&buffer, length);

    if (stream == NULL) {
        return NULL;
    }

    ir_program_dump(stream, program);
    fclose(stream);
    return buffer;
}

static void run(const Ir_Program *program, Ir_Interp_Result *result) {
    FILE *sink = fuzz_sink();

    memset(result, 0, sizeof(*result));

    if (sink == NULL) {
        return;
    }

    ir_interp_run(program, sink, FUZZ_INTERP_STEPS, result);
    fuzz_check(result->steps <= FUZZ_INTERP_STEPS);
    fuzz_check(result->status != IR_INTERP_EXITED ||
               (result->exit_code >= 0 && result->exit_code <= 255));
}

/* A verified program: round trip, run, optimize. */
static void fuzz_valid(Ir_Program *program) {
    Diagnostic error;
    Ir_Interp_Result first;
    size_t length = 0;
    char *dumped = dump_program(program, &length);

    run(program, &first);

    if (dumped != NULL) {
        Source source;

        if (source_init(&source, "fuzz-dump.ir", dumped, length)) {
            Ir_Program again;

            fuzz_check(ir_parse(&source, &again, &error));
            fuzz_check(ir_verify(&again, &error));

            size_t relength = 0;
            char *redumped = dump_program(&again, &relength);

            if (redumped != NULL) {
                fuzz_check(relength == length);
                fuzz_check(memcmp(redumped, dumped, length) == 0);
            }

            Ir_Interp_Result second;
            run(&again, &second);

            if (first.status != IR_INTERP_OUT_OF_MEMORY &&
                second.status != IR_INTERP_OUT_OF_MEMORY) {
                fuzz_check(first.status == second.status);
                fuzz_check(first.steps == second.steps);
                fuzz_check(first.exit_code == second.exit_code);
            }

            free(redumped);
            ir_program_free(&again);
            source_free(&source);
        }
    }

    free(dumped);

    /* §5.7, §8.7: -O1 verifies, behaves the same, and is a fixpoint. */
    fuzz_optimizer(program, FUZZ_INTERP_STEPS);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Source source;
    Ir_Program program;
    Diagnostic error;

    if (!source_init(&source, "fuzz.ir", (const char *)data, size)) {
        return 0;
    }

    if (ir_parse(&source, &program, &error)) {
        if (ir_verify(&program, &error)) {
            fuzz_check(error.message[0] == '\0');
            fuzz_valid(&program);
        } else {
            fuzz_check(error.stage == DIAGNOSTIC_VERIFY);
            fuzz_check(error.message[0] != '\0');
        }
    } else {
        fuzz_check(error.stage == DIAGNOSTIC_IR);
        fuzz_check(error.message[0] != '\0');
        fuzz_check(program.block_count == 0);
    }

    ir_program_free(&program);
    ir_program_free(&program);
    source_free(&source);
    return 0;
}
