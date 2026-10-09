#include "symbol.h"

#include <stdlib.h>
#include <string.h>

/* ASCII only, independent of the C locale (§2.4: identifiers are ASCII). */
static unsigned char fold(char c) {
    const unsigned char byte = (unsigned char)c;
    return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte - 'A' + 'a')
                                      : byte;
}

bool symbol_name_equals(
    const char *spelling,
    size_t length,
    const char *name
) {
    for (size_t i = 0; i < length; ++i) {
        if (name[i] == '\0' || fold(spelling[i]) != (unsigned char)name[i]) {
            return false;
        }
    }

    return name[length] == '\0';
}

/* FNV-1a over the case-folded bytes. */
static size_t hash_name(const char *spelling, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);

    for (size_t i = 0; i < length; ++i) {
        hash ^= fold(spelling[i]);
        hash *= UINT64_C(1099511628211);
    }

    return (size_t)hash;
}

void symbol_table_init(Symbol_Table *table) {
    memset(table, 0, sizeof(*table));
}

void symbol_table_free(Symbol_Table *table) {
    for (size_t i = 0; i < table->count; ++i) {
        free(table->symbols[i].name);
    }

    free(table->symbols);
    free(table->buckets);
    symbol_table_init(table);
}

/* The bucket holding 'spelling', or the empty bucket where it belongs.
 * bucket_count must be nonzero. */
static size_t find_bucket(
    const Symbol_Table *table,
    const char *spelling,
    size_t length
) {
    const size_t mask = table->bucket_count - 1;
    size_t index = hash_name(spelling, length) & mask;

    for (;;) {
        const size_t entry = table->buckets[index];

        if (entry == 0 ||
            (table->symbols[entry - 1].name_length == length &&
             symbol_name_equals(spelling, length,
                                table->symbols[entry - 1].name))) {
            return index;
        }

        index = (index + 1) & mask;
    }
}

size_t symbol_table_find(
    const Symbol_Table *table,
    const char *spelling,
    size_t length
) {
    if (table->bucket_count == 0) {
        return SYMBOL_NONE;
    }

    const size_t entry =
        table->buckets[find_bucket(table, spelling, length)];
    return entry == 0 ? SYMBOL_NONE : entry - 1;
}

/* Keeps the load factor at most 1/2 after one more insertion. */
static bool reserve(Symbol_Table *table) {
    if (table->count == table->capacity) {
        if (table->capacity > SIZE_MAX / 2 / sizeof(Symbol)) {
            return false;
        }

        const size_t capacity = table->capacity == 0 ? 16 : table->capacity * 2;
        Symbol *grown = realloc(table->symbols, capacity * sizeof(Symbol));

        if (grown == NULL) {
            return false;
        }

        table->symbols = grown;
        table->capacity = capacity;
    }

    if ((table->count + 1) * 2 <= table->bucket_count) {
        return true;
    }

    const size_t old_count = table->bucket_count;
    size_t *old_buckets = table->buckets;
    const size_t bucket_count = old_count == 0 ? 32 : old_count * 2;

    if (bucket_count > SIZE_MAX / sizeof(size_t)) {
        return false;
    }

    size_t *buckets = calloc(bucket_count, sizeof(size_t));

    if (buckets == NULL) {
        return false;
    }

    table->buckets = buckets;
    table->bucket_count = bucket_count;

    for (size_t i = 0; i < old_count; ++i) {
        if (old_buckets[i] != 0) {
            const Symbol *symbol = &table->symbols[old_buckets[i] - 1];
            buckets[find_bucket(table, symbol->name, symbol->name_length)] =
                old_buckets[i];
        }
    }

    free(old_buckets);
    return true;
}

bool symbol_table_intern(
    Symbol_Table *table,
    const char *spelling,
    size_t length,
    Source_Span span,
    size_t *id
) {
    const size_t existing = symbol_table_find(table, spelling, length);

    if (existing != SYMBOL_NONE) {
        *id = existing;
        return true;
    }

    if (length == SIZE_MAX || !reserve(table)) {
        return false;
    }

    char *name = malloc(length + 1);

    if (name == NULL) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        name[i] = (char)fold(spelling[i]);
    }

    name[length] = '\0';

    Symbol *symbol = &table->symbols[table->count];
    memset(symbol, 0, sizeof(*symbol));
    symbol->name = name;
    symbol->name_length = length;
    symbol->first_span = span;

    table->buckets[find_bucket(table, spelling, length)] = table->count + 1;
    *id = table->count;
    ++table->count;
    return true;
}
