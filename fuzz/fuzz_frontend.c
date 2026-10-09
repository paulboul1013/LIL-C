#define _POSIX_C_SOURCE 200809L

/*
 * libFuzzer harness for the LIL-C front end (lexer, parser and semantic
 * analysis) and the IR stages that follow it (lowering, verifier, text
 * format, interpreter, -O1 optimizer).
 *
 * Contract (docs/spec.md §6.3): for any input bytes the front end either
 * succeeds or reports a diagnostic. It must never crash, read out of
 * bounds, trigger undefined behaviour, leak, or loop forever.
 *
 * Build and run with 'make fuzz' / 'make fuzz-run FUZZ_TIME=60'.
 */
#include "ast_debug.h"
#include "ast_print.h"
#include "ir.h"
#include "ir_interp.h"
#include "ir_lower.h"
#include "ir_parse.h"
#include "ir_verify.h"
#include "lexer.h"
#include "lil.h"
#include "sema.h"
#include "token_debug.h"

#include "fuzz_opt.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void fuzz_check(int condition) {
    if (!condition) {
        abort();
    }
}

/* The raw lexer must reach TOKEN_END or TOKEN_INVALID, with in-bounds,
 * strictly advancing spans. */
static void fuzz_lexer(const char *text, size_t size) {
    Source source;

    if (!source_init(&source, "fuzz.lil", text, size)) {
        return;
    }

    Lexer lexer;
    lexer_init(&lexer, &source);

    size_t previous_end = 0;

    for (size_t steps = 0; steps <= size + 1; ++steps) {
        const Token token = lexer_next_token(&lexer);

        fuzz_check(token.span.offset >= previous_end);
        fuzz_check(token.span.offset <= size);
        fuzz_check(token.span.length <= size - token.span.offset);
        fuzz_check(token.span.line >= 1 && token.span.column >= 1);
        previous_end = token.span.offset + token.span.length;

        if (token.kind == TOKEN_END) {
            fuzz_check(token.span.length == 0);
            fuzz_check(!diagnostic_has_error(lexer_diagnostic(&lexer)));
            source_free(&source);
            return;
        }

        if (token.kind == TOKEN_INVALID) {
            fuzz_check(diagnostic_has_error(lexer_diagnostic(&lexer)));
            source_free(&source);
            return;
        }

        fuzz_check(token.span.length > 0);

        /* §2.6, §2.7: literal values are in range; other kinds carry 0. */
        if (token.kind == TOKEN_NUMBER) {
            fuzz_check(token.value >= 0);
        } else if (token.kind == TOKEN_CHARACTER) {
            fuzz_check(token.value >= 0 && token.value <= 255);
        } else {
            fuzz_check(token.value == 0);
        }
    }

    /* Every non-final token consumes at least one byte, so the loop above
     * must have terminated. */
    fuzz_check(0);
}

static FILE *fuzz_sink(void) {
    static FILE *sink;

    if (sink == NULL) {
        sink = fopen("/dev/null", "w");
    }

    return sink;
}

/* --dump-tokens (§6.4.1) must agree with the raw lexer about success. */
static void fuzz_token_dump(const char *text, size_t size) {
    Source source;
    FILE *sink = fuzz_sink();

    if (sink == NULL || !source_init(&source, "fuzz.lil", text, size)) {
        return;
    }

    Diagnostic diagnostic;

    if (token_debug_dump(sink, &source, &diagnostic)) {
        fuzz_check(!diagnostic_has_error(&diagnostic));
    } else {
        fuzz_check(diagnostic.stage == DIAGNOSTIC_LEXICAL);
        fuzz_check(diagnostic.span.offset <= size);
    }

    source_free(&source);
}

/* Round trip (plan stage 2): an accepted program printed back to source
 * must parse again, and printing that program must give the same text. */
static void fuzz_round_trip(const Lil_Result *result) {
    size_t length = 0;
    char *printed =
        ast_print_program(&result->source, &result->program, &length);

    if (printed == NULL) {
        return;     /* out of memory */
    }

    Lil_Result again;
    fuzz_check(lil_run_source("printed.lil", printed, length, &again));
    fuzz_check(again.program.statement_count ==
               result->program.statement_count);

    size_t reprinted_length = 0;
    char *reprinted =
        ast_print_program(&again.source, &again.program, &reprinted_length);

    if (reprinted != NULL) {
        fuzz_check(reprinted_length == length);
        fuzz_check(memcmp(reprinted, printed, length) == 0);
    }

    free(reprinted);
    lil_result_free(&again);
    free(printed);
}

/* Instructions the interpreter may run per input: enough for small loops,
 * small enough that 'while (1);' cannot slow the fuzzer down. */
#define FUZZ_INTERP_STEPS 10000

/*
 * Stage 4: every analyzed program lowers to IR that passes ir_verify()
 * (§8.4, §8.5); its dump parses back to the same dump (§8.3); and the
 * interpreter runs it within a small step limit without crashing (§8.6);
 * -O1 keeps it valid and behaving the same (§5.7, §8.7, fuzz_opt.h).
 */
static void fuzz_ir(const Lil_Result *result) {
    Ir_Program program;
    Diagnostic error;

    if (!ir_lower_program(&result->program, &result->symbols, &program)) {
        return;     /* out of memory */
    }

    fuzz_check(ir_verify(&program, &error));

    char *dumped = NULL;
    size_t length = 0;
    FILE *stream = open_memstream(&dumped, &length);

    if (stream != NULL) {
        ir_program_dump(stream, &program);
        fclose(stream);

        Source source;

        if (dumped != NULL &&
            source_init(&source, "fuzz.ir", dumped, length)) {
            Ir_Program again;

            fuzz_check(ir_parse(&source, &again, &error));
            fuzz_check(ir_verify(&again, &error));

            char *redumped = NULL;
            size_t relength = 0;
            FILE *restream = open_memstream(&redumped, &relength);

            if (restream != NULL) {
                ir_program_dump(restream, &again);
                fclose(restream);
                fuzz_check(relength == length);
                fuzz_check(memcmp(redumped, dumped, length) == 0);
            }

            free(redumped);
            ir_program_free(&again);
            source_free(&source);
        }
    }

    free(dumped);

    FILE *sink = fuzz_sink();

    if (sink != NULL) {
        Ir_Interp_Result run;

        ir_interp_run(&program, sink, FUZZ_INTERP_STEPS, &run);
        fuzz_check(run.steps <= FUZZ_INTERP_STEPS);
        fuzz_check(run.status != IR_INTERP_EXITED ||
                   (run.exit_code >= 0 && run.exit_code <= 255));
    }

    /* §5.7, §8.7: -O1 verifies, behaves the same, and is a fixpoint. */
    fuzz_optimizer(&program, FUZZ_INTERP_STEPS);
    ir_program_free(&program);
}

/* Diagnostics in strictly increasing source order, all in bounds. */
static void fuzz_check_ordered(const Diagnostic_List *list, size_t size) {
    for (size_t i = 0; i < list->count; ++i) {
        fuzz_check(list->items[i].span.offset <= size);

        if (i > 0) {
            fuzz_check(list->items[i].span.offset >
                       list->items[i - 1].span.offset);
        }
    }
}

/*
 * Stage 3: the whole front end must agree with the syntactic one, report
 * only semantic (or internal) errors on a program that parses, and hand
 * later stages an AST that satisfies the post-sema invariants
 * (include/sema.h).
 */
static void fuzz_sema(const char *text, size_t size, bool parsed) {
    Lil_Result result;
    const bool succeeded = lil_analyze_source("fuzz.lil", text, size, &result);

    fuzz_check(result.warnings.count <= DIAGNOSTIC_MAX_WARNINGS);
    fuzz_check_ordered(&result.warnings, size);

    for (size_t i = 0; i < result.warnings.count; ++i) {
        fuzz_check(diagnostic_is_warning(&result.warnings.items[i]));
    }

    if (succeeded) {
        fuzz_check(parsed);
        fuzz_check(result.has_program && result.is_analyzed);
        fuzz_check(result.errors.count == 0);
        fuzz_check(sema_verify(&result.program, &result.source,
                               &result.symbols));
        fuzz_ir(&result);

        FILE *sink = fuzz_sink();

        if (sink != NULL) {
            ast_debug_print_program(sink, &result.source, &result.program);
            lil_result_fprint_diagnostics(sink, &result);
        }
    } else {
        fuzz_check(!result.has_program && !result.is_analyzed);
        fuzz_check(result.errors.count >= 1);
        fuzz_check(result.errors.count <= DIAGNOSTIC_MAX_ERRORS);
        fuzz_check_ordered(&result.errors, size);

        for (size_t i = 0; i < result.errors.count; ++i) {
            const Diagnostic_Stage stage = result.errors.items[i].stage;

            fuzz_check(diagnostic_has_error(&result.errors.items[i]));
            /* Semantic errors exactly when the program parsed. */
            if (parsed) {
                fuzz_check(stage == DIAGNOSTIC_SEMANTIC ||
                           stage == DIAGNOSTIC_INTERNAL);
            } else {
                fuzz_check(stage != DIAGNOSTIC_SEMANTIC);
            }
        }

        /* Warnings only come from analysis, which needs a program. */
        if (!parsed) {
            fuzz_check(result.warnings.count == 0);
        }
    }

    lil_result_free(&result);
    lil_result_free(&result);
}

static void fuzz_pipeline(const char *text, size_t size) {
    Lil_Result result;
    const bool succeeded = lil_run_source("fuzz.lil", text, size, &result);

    if (succeeded) {
        FILE *sink = fuzz_sink();

        fuzz_check(result.has_program);
        fuzz_check(!diagnostic_has_error(&result.diagnostic));

        /* Every accepted AST must also be printable (recursive pass). */

        if (sink != NULL) {
            ast_debug_print_program(sink, &result.source, &result.program);
        }

        fuzz_check(result.errors.count == 0);
        fuzz_round_trip(&result);
    } else {
        fuzz_check(!result.has_program);
        fuzz_check(diagnostic_has_error(&result.diagnostic));
        fuzz_check(result.diagnostic.span.offset <= size);

        /* §6.3.4: 1..20 errors in strictly increasing source order, the
         * first one mirrored in result.diagnostic, none after a lexical
         * error. */
        fuzz_check(result.errors.count >= 1);
        fuzz_check(result.errors.count <= DIAGNOSTIC_MAX_ERRORS);
        fuzz_check(result.errors.items[0].span.offset ==
                   result.diagnostic.span.offset);

        for (size_t i = 0; i < result.errors.count; ++i) {
            const Diagnostic *error = &result.errors.items[i];

            fuzz_check(diagnostic_has_error(error));
            fuzz_check(error->span.offset <= size);

            if (i > 0) {
                fuzz_check(error->span.offset >
                           result.errors.items[i - 1].span.offset);
            }

            if (error->stage == DIAGNOSTIC_LEXICAL) {
                fuzz_check(i + 1 == result.errors.count);
            }
        }
    }

    lil_result_free(&result);
    lil_result_free(&result);
    fuzz_sema(text, size, succeeded);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *text = (const char *)data;

    fuzz_lexer(text, size);
    fuzz_token_dump(text, size);
    fuzz_pipeline(text, size);
    return 0;
}
