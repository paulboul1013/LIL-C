#ifndef LIL_DIAGNOSTIC_H
#define LIL_DIAGNOSTIC_H

#include "source.h"

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    DIAGNOSTIC_NONE,
    DIAGNOSTIC_LEXICAL,
    DIAGNOSTIC_SYNTAX,
    DIAGNOSTIC_INTERNAL
} Diagnostic_Stage;

typedef struct {
    Diagnostic_Stage stage;
    Source_Span span;
    char message[256];
} Diagnostic;

void diagnostic_clear(Diagnostic *diagnostic);
void diagnostic_set(
    Diagnostic *diagnostic,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *format,
    ...
);
bool diagnostic_has_error(const Diagnostic *diagnostic);
void diagnostic_fprint(
    FILE *stream,
    const Source *source,
    const Diagnostic *diagnostic
);

#endif
