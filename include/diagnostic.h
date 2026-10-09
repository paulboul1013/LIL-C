#ifndef LIL_DIAGNOSTIC_H
#define LIL_DIAGNOSTIC_H

#include "source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
    DIAGNOSTIC_NONE,
    DIAGNOSTIC_LEXICAL,
    DIAGNOSTIC_SYNTAX,
    DIAGNOSTIC_SEMANTIC,
    DIAGNOSTIC_INTERNAL,
    /* IR text that ir_parse() rejects (docs/spec.md §8.3). */
    DIAGNOSTIC_IR,
    /* An IR program that ir_verify() rejects (§8.4). */
    DIAGNOSTIC_VERIFY
} Diagnostic_Stage;

/* §6.1 errors stop compilation; §6.2 warnings are reported and ignored. */
typedef enum {
    DIAGNOSTIC_ERROR,
    DIAGNOSTIC_WARNING
} Diagnostic_Severity;

typedef struct {
    Diagnostic_Stage stage;
    Diagnostic_Severity severity;
    Source_Span span;
    char message[256];
} Diagnostic;

void diagnostic_clear(Diagnostic *diagnostic);
/* Sets an error (DIAGNOSTIC_ERROR) with a printf-style message. */
void diagnostic_set(
    Diagnostic *diagnostic,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *format,
    ...
)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
;
/* Like diagnostic_set(), but the severity is DIAGNOSTIC_WARNING. */
void diagnostic_set_warning(
    Diagnostic *diagnostic,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *format,
    ...
)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
;
/* True for a set diagnostic whose severity is DIAGNOSTIC_ERROR. */
bool diagnostic_has_error(const Diagnostic *diagnostic);
bool diagnostic_is_warning(const Diagnostic *diagnostic);
void diagnostic_fprint(
    FILE *stream,
    const Source *source,
    const Diagnostic *diagnostic
);

/* §6.3.4: at most this many errors are reported per compilation. */
#define DIAGNOSTIC_MAX_ERRORS 20
/* §6.2: at most this many warnings (the first ones in source order). */
#define DIAGNOSTIC_MAX_WARNINGS 20

/*
 * Diagnostics in the order they were reported (strictly increasing source
 * order, §6.1, §6.2). One type holds both errors and warnings, so its
 * capacity -- the array length, which diagnostic_list_add() enforces -- is
 * the limit of either. The spec gives both limits the same value; if they
 * ever differ, give the list a per-instance limit instead of relaxing this
 * assertion.
 */
#define DIAGNOSTIC_LIST_CAPACITY DIAGNOSTIC_MAX_ERRORS

_Static_assert(DIAGNOSTIC_MAX_WARNINGS == DIAGNOSTIC_LIST_CAPACITY,
               "Diagnostic_List holds warnings too: its capacity must be "
               "the warning limit");

typedef struct {
    Diagnostic items[DIAGNOSTIC_LIST_CAPACITY];
    size_t count;
} Diagnostic_List;

void diagnostic_list_clear(Diagnostic_List *list);
/* Appends a copy of diagnostic; returns false (and drops it) when full. */
bool diagnostic_list_add(Diagnostic_List *list, const Diagnostic *diagnostic);
bool diagnostic_list_is_full(const Diagnostic_List *list);
void diagnostic_list_fprint(
    FILE *stream,
    const Source *source,
    const Diagnostic_List *list
);
/* Prints both lists merged by source offset (errors first on a tie). */
void diagnostic_lists_fprint_merged(
    FILE *stream,
    const Source *source,
    const Diagnostic_List *errors,
    const Diagnostic_List *warnings
);

#endif
