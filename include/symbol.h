#ifndef LIL_SYMBOL_H
#define LIL_SYMBOL_H

#include "source.h"
#include "symbol_id.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Symbol table (docs/spec.md §5.1, §5.2). Every variable of the program
 * gets one entry; ids are 0..count-1 in order of first appearance in the
 * source. Names are compared after ASCII case folding, so "X" and "x" are
 * the same symbol. The builtin 'putc' is never a symbol (§5.4).
 */

typedef struct {
    /* ASCII-lowercased name, NUL-terminated (identifiers are ASCII). */
    char *name;
    size_t name_length;
    /* The first occurrence (read or write), with its original spelling. */
    Source_Span first_span;
    /* Number of reads (variable references) and writes (assignments). */
    size_t read_count;
    size_t write_count;
    /* The target of the first assignment; valid when write_count > 0. */
    Source_Span first_write_span;
} Symbol;

typedef struct {
    Symbol *symbols;        /* indexed by id */
    size_t count;
    size_t capacity;
    size_t *buckets;        /* open addressing: id + 1, 0 = empty */
    size_t bucket_count;    /* 0 or a power of two */
} Symbol_Table;

void symbol_table_init(Symbol_Table *table);
/* Releases everything and re-initializes; safe to call repeatedly. */
void symbol_table_free(Symbol_Table *table);

/* The id of the symbol named 'spelling' (any case), or SYMBOL_NONE. */
size_t symbol_table_find(
    const Symbol_Table *table,
    const char *spelling,
    size_t length
);

/*
 * Finds the symbol named 'spelling' (any case) or creates it with
 * first_span = span. Stores its id in *id. Returns false only when out of
 * memory (the table is then unchanged).
 */
bool symbol_table_intern(
    Symbol_Table *table,
    const char *spelling,
    size_t length,
    Source_Span span,
    size_t *id
);

/* §5.2: ASCII case-insensitive comparison of a spelling with a name. */
bool symbol_name_equals(
    const char *spelling,
    size_t length,
    const char *name
);

#endif
