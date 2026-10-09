#ifndef LIL_FUZZ_OPT_H
#define LIL_FUZZ_OPT_H

/*
 * Shared by the fuzz harnesses: the -O1 differential check (docs/spec.md
 * §5.7, §8.7) on one valid IR program.
 *
 *   copy = program; ir_optimize(copy, 1); ir_verify(copy);
 *   interpret both with a small step limit and compare:
 *     - the unoptimized run ended (exit or trap): the optimized one ends
 *       the same way, with the same exit code and output bytes, in at
 *       most as many steps (no pass lengthens an execution);
 *     - the unoptimized run hit the step limit: its output is a prefix of
 *       the optimized run's output (which got at least as far);
 *   and -O1 again changes nothing (the fixpoint is idempotent).
 *
 * ir_optimize() must succeed and reach its fixpoint (no "stopped after
 * N rounds" warning); any other failure -- a verifier failure after a
 * pass is a compiler bug -- aborts. Out of memory anywhere skips the rest
 * of the check.
 */

#include "ir.h"
#include "ir_interp.h"
#include "ir_opt.h"
#include "ir_parse.h"
#include "ir_verify.h"
#include "source.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fuzz_opt_check(int condition) {
    if (!condition) {
        abort();
    }
}

/* ir_optimize() once more on 'program': true when it succeeded (and then
 * reached its fixpoint), false only when it ran out of memory. */
static int fuzz_opt_optimize(Ir_Program *program) {
    Diagnostic error;

    if (!ir_optimize(program, 1, &error)) {
        fuzz_opt_check(strstr(error.message, "out of memory") != NULL);
        return 0;
    }

    fuzz_opt_check(!diagnostic_is_warning(&error));
    return 1;
}

/* The §8.3 dump as a malloc'ed buffer (NULL when out of memory). */
static char *fuzz_opt_dump(const Ir_Program *program, size_t *length) {
    char *buffer = NULL;
    FILE *stream = open_memstream(&buffer, length);

    if (stream == NULL) {
        return NULL;
    }

    ir_program_dump(stream, program);
    fclose(stream);
    return buffer;
}

typedef struct {
    Ir_Interp_Result result;
    char *output;
    size_t length;
    int ok;
} Fuzz_Opt_Run;

static void fuzz_opt_run(const Ir_Program *program, uint64_t steps,
                         Fuzz_Opt_Run *run) {
    FILE *stream = open_memstream(&run->output, &run->length);

    memset(&run->result, 0, sizeof(run->result));
    run->ok = 0;

    if (stream == NULL) {
        run->output = NULL;
        return;
    }

    ir_interp_run(program, stream, steps, &run->result);
    fclose(stream);
    run->ok = run->output != NULL &&
              run->result.status != IR_INTERP_OUT_OF_MEMORY;
}

static int fuzz_opt_ended(const Fuzz_Opt_Run *run) {
    return run->result.status == IR_INTERP_EXITED ||
           run->result.status == IR_INTERP_TRAPPED;
}

static void fuzz_optimizer(const Ir_Program *program, uint64_t steps) {
    Diagnostic error;
    size_t length = 0;
    char *text = fuzz_opt_dump(program, &length);
    Source source;
    Ir_Program copy;

    if (text == NULL || !source_init(&source, "fuzz-opt.ir", text, length)) {
        free(text);
        return;
    }

    fuzz_opt_check(ir_parse(&source, &copy, &error));

    if (fuzz_opt_optimize(&copy)) {
        fuzz_opt_check(ir_verify(&copy, &error));

        Fuzz_Opt_Run before;
        Fuzz_Opt_Run after;

        fuzz_opt_run(program, steps, &before);
        fuzz_opt_run(&copy, steps, &after);

        if (before.ok && after.ok) {
            if (fuzz_opt_ended(&before)) {
                fuzz_opt_check(after.result.status == before.result.status);
                fuzz_opt_check(after.result.exit_code ==
                               before.result.exit_code);
                fuzz_opt_check(after.length == before.length);
                fuzz_opt_check(memcmp(after.output, before.output,
                                      before.length) == 0);
                fuzz_opt_check(after.result.steps <= before.result.steps);
            } else {
                fuzz_opt_check(before.length <= after.length);
                fuzz_opt_check(memcmp(after.output, before.output,
                                      before.length) == 0);
            }
        }

        free(before.output);
        free(after.output);

        size_t once_length = 0;
        char *once = fuzz_opt_dump(&copy, &once_length);

        if (once != NULL && fuzz_opt_optimize(&copy)) {
            size_t twice_length = 0;
            char *twice = fuzz_opt_dump(&copy, &twice_length);

            if (twice != NULL) {
                fuzz_opt_check(twice_length == once_length);
                fuzz_opt_check(memcmp(twice, once, once_length) == 0);
            }

            free(twice);
        }

        free(once);
    }

    ir_program_free(&copy);
    source_free(&source);
    free(text);
}

#endif
