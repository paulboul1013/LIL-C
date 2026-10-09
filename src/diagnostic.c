#include "diagnostic.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void diagnostic_clear(Diagnostic *diagnostic) {
    memset(diagnostic, 0, sizeof(*diagnostic));
}

static void diagnostic_vset(
    Diagnostic *diagnostic,
    Diagnostic_Stage stage,
    Diagnostic_Severity severity,
    Source_Span span,
    const char *format,
    va_list args
) {
    diagnostic->stage = stage;
    diagnostic->severity = severity;
    diagnostic->span = span;
    vsnprintf(
        diagnostic->message,
        sizeof(diagnostic->message),
        format,
        args
    );
}

void diagnostic_set(
    Diagnostic *diagnostic,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *format,
    ...
) {
    va_list args;
    va_start(args, format);
    diagnostic_vset(diagnostic, stage, DIAGNOSTIC_ERROR, span, format, args);
    va_end(args);
}

void diagnostic_set_warning(
    Diagnostic *diagnostic,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *format,
    ...
) {
    va_list args;
    va_start(args, format);
    diagnostic_vset(diagnostic, stage, DIAGNOSTIC_WARNING, span, format, args);
    va_end(args);
}

bool diagnostic_has_error(const Diagnostic *diagnostic) {
    return diagnostic->stage != DIAGNOSTIC_NONE &&
           diagnostic->severity == DIAGNOSTIC_ERROR;
}

bool diagnostic_is_warning(const Diagnostic *diagnostic) {
    return diagnostic->stage != DIAGNOSTIC_NONE &&
           diagnostic->severity == DIAGNOSTIC_WARNING;
}

static const char *diagnostic_stage_name(Diagnostic_Stage stage) {
    switch (stage) {
        case DIAGNOSTIC_LEXICAL:
            return "lexical";
        case DIAGNOSTIC_SYNTAX:
            return "syntax";
        case DIAGNOSTIC_SEMANTIC:
            return "semantic";
        case DIAGNOSTIC_INTERNAL:
            return "internal";
        case DIAGNOSTIC_IR:
            return "ir";
        case DIAGNOSTIC_VERIFY:
            return "verify";
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
    const char *path = source->path == NULL ? "<source>" : source->path;

    /* §6.2: warnings carry no stage name. */
    if (diagnostic->severity == DIAGNOSTIC_WARNING) {
        fprintf(
            stream,
            "%s:%zu:%zu: warning: %s\n",
            path,
            diagnostic->span.line,
            diagnostic->span.column,
            diagnostic->message
        );
        return;
    }

    fprintf(
        stream,
        "%s:%zu:%zu: %s error: %s\n",
        path,
        diagnostic->span.line,
        diagnostic->span.column,
        diagnostic_stage_name(diagnostic->stage),
        diagnostic->message
    );
}

void diagnostic_list_clear(Diagnostic_List *list) {
    memset(list, 0, sizeof(*list));
}

bool diagnostic_list_is_full(const Diagnostic_List *list) {
    return list->count >= DIAGNOSTIC_LIST_CAPACITY;
}

bool diagnostic_list_add(Diagnostic_List *list, const Diagnostic *diagnostic) {
    if (diagnostic_list_is_full(list)) {
        return false;
    }

    list->items[list->count] = *diagnostic;
    ++list->count;
    return true;
}

void diagnostic_list_fprint(
    FILE *stream,
    const Source *source,
    const Diagnostic_List *list
) {
    for (size_t i = 0; i < list->count; ++i) {
        diagnostic_fprint(stream, source, &list->items[i]);
    }
}

void diagnostic_lists_fprint_merged(
    FILE *stream,
    const Source *source,
    const Diagnostic_List *errors,
    const Diagnostic_List *warnings
) {
    size_t e = 0;
    size_t w = 0;

    while (e < errors->count || w < warnings->count) {
        const bool take_error =
            w >= warnings->count ||
            (e < errors->count &&
             errors->items[e].span.offset <= warnings->items[w].span.offset);

        if (take_error) {
            diagnostic_fprint(stream, source, &errors->items[e++]);
        } else {
            diagnostic_fprint(stream, source, &warnings->items[w++]);
        }
    }
}
