#ifndef LIL_SOURCE_H
#define LIL_SOURCE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t offset;
    size_t length;
    size_t line;
    size_t column;
} Source_Span;

typedef struct {
    char *path;
    char *text;
    size_t length;
} Source;

bool source_init(
    Source *source,
    const char *path,
    const char *text,
    size_t length
);

/* source_init() copies path and text; release both with source_free(). */
void source_free(Source *source);
const char *source_text_at(const Source *source, Source_Span span);

#endif
