#include "lil.h"

#include "lexer.h"

#include <string.h>

bool lil_run_source(
    const char *path,
    const char *text,
    size_t length,
    Lil_Result *result
) {
    memset(result, 0, sizeof(*result));

    if (!source_init(&result->source, path, text, length)) {
        diagnostic_set(
            &result->diagnostic,
            DIAGNOSTIC_INTERNAL,
            (Source_Span){0},
            "out of memory while copying source"
        );
        return false;
    }

    Lexer lexer;
    lexer_init(&lexer, &result->source);

    Parser *parser = parser_create(&lexer);

    if (parser == NULL) {
        diagnostic_set(
            &result->diagnostic,
            DIAGNOSTIC_INTERNAL,
            (Source_Span){0},
            "out of memory while creating parser"
        );
        return false;
    }

    if (!parser_parse_program(parser, &result->program)) {
        result->diagnostic = *parser_diagnostic(parser);
        parser_destroy(parser);
        return false;
    }

    parser_destroy(parser);
    result->has_program = true;
    return true;
}

void lil_result_free(Lil_Result *result) {
    program_free(&result->program);
    source_free(&result->source);
    result->has_program = false;
    diagnostic_clear(&result->diagnostic);
}
