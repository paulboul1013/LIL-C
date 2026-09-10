#ifndef LIL_H
#define LIL_H

#include "diagnostic.h"
#include "parser.h"
#include "source.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    Source source;
    Program program;
    Diagnostic diagnostic;
    bool has_program;
} Lil_Result;

/*
 * Copies path and text into result. Call lil_result_free() after every call,
 * whether lil_run_source() succeeds or fails.
 */
bool lil_run_source(
    const char *path,
    const char *text,
    size_t length,
    Lil_Result *result
);
void lil_result_free(Lil_Result *result);

#endif
