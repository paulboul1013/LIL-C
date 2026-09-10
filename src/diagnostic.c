#include "diagnostic.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diagnostic_clear(Diagnostic *diagnostic) {
    memset(diagnostic, 0, sizeof(*diagnostic));
}

void diagnostic_set(
    Diagnostic *diagnostic,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *format,
    ...
) {
    diagnostic->stage = stage;
    diagnostic->span = span;

    va_list args;
    va_start(args, format);
    vsnprintf(
        diagnostic->message,
        sizeof(diagnostic->message),
        format,
        args
    );
    va_end(args);
}

bool diagnostic_has_error(const Diagnostic *diagnostic) {
    return diagnostic->stage != DIAGNOSTIC_NONE;
}

static const char *diagnostic_stage_name(Diagnostic_Stage stage) {
    switch (stage) {
        case DIAGNOSTIC_LEXICAL:
            return "lexical";
        case DIAGNOSTIC_SYNTAX:
            return "syntax";
        case DIAGNOSTIC_INTERNAL:
            return "internal";
        case DIAGNOSTIC_NONE:
            return "unknown";
    }

    return "unknown";
}

void diagnostic_fprint(
    FILE *stream,
    const Source *source,
    const Diagnostic *diagnostic
) {
    fprintf(
        stream,
        "%s:%zu:%zu: %s error: %s\n",
        source->path == NULL ? "<source>" : source->path,
        diagnostic->span.line,
        diagnostic->span.column,
        diagnostic_stage_name(diagnostic->stage),
        diagnostic->message
    );
}
