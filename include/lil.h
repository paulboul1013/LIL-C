#ifndef LIL_H
#define LIL_H

#include "diagnostic.h"
#include "parser.h"
#include "source.h"
#include "symbol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    Source source;
    Program program;
    /* The first error (empty on success); equal to errors.items[0]. */
    Diagnostic diagnostic;
    /* Every reported error in source order (§6.3.4). */
    Diagnostic_List errors;
    /* §6.2 warnings in source order (lil_analyze_source() only). */
    Diagnostic_List warnings;
    /* Every variable; filled by lil_analyze_source() (include/sema.h). */
    Symbol_Table symbols;
    bool has_program;
    /* has_program, and the program passed semantic analysis: it satisfies
     * the post-sema invariants of include/sema.h. */
    bool is_analyzed;
} Lil_Result;

/*
 * The syntactic front end: lexer and parser only (no semantic checks, so
 * for example "f(1);" is accepted). Copies path and text into result.
 * Call lil_result_free() after every call, whether it succeeds or fails.
 */
bool lil_run_source(
    const char *path,
    const char *text,
    size_t length,
    Lil_Result *result
);

/*
 * The whole front end: lil_run_source() followed by semantic analysis
 * (include/sema.h). Returns true when there was no error; warnings may
 * still have been reported. On success result->is_analyzed is true and
 * result->symbols holds every variable. On failure there is no program
 * (has_program is false), and errors/warnings hold the diagnostics.
 * Call lil_result_free() afterwards either way.
 */
bool lil_analyze_source(
    const char *path,
    const char *text,
    size_t length,
    Lil_Result *result
);

/* Every error and warning, merged in source order (§6.1, §6.2). */
void lil_result_fprint_diagnostics(FILE *stream, const Lil_Result *result);

void lil_result_free(Lil_Result *result);

#endif
