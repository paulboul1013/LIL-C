#include "source.h"

#include <stdlib.h>
#include <string.h>

bool source_init(
    Source *source,
    const char *path,
    const char *text,
    size_t length
) {
    const size_t path_length = strlen(path);

    source->path = malloc(path_length + 1);
    source->text = malloc(length + 1);
    source->length = 0;

    if (source->path == NULL || source->text == NULL) {
        source_free(source);
        return false;
    }

    memcpy(source->path, path, path_length + 1);
    memcpy(source->text, text, length);
    source->text[length] = '\0';
    source->length = length;
    return true;
}

void source_free(Source *source) {
    free(source->path);
    free(source->text);
    source->path = NULL;
    source->text = NULL;
    source->length = 0;
}

const char *source_text_at(const Source *source, Source_Span span) {
    if (span.offset > source->length ||
        span.length > source->length - span.offset) {
        return "";
    }

    return source->text + span.offset;
}
