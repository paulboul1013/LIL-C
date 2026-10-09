#include "lil.h"

#include "lexer.h"
#include "sema.h"

#include <string.h>

/* Records an internal error that happened before parsing started. */
static void lil_result_fail(Lil_Result *result, const char *message) {
    diagnostic_set(
        &result->diagnostic,
        DIAGNOSTIC_INTERNAL,
        (Source_Span){0, 0, 1, 1},
        "%s",
        message
    );
    diagnostic_list_add(&result->errors, &result->diagnostic);
}

bool lil_run_source(
    const char *path,
    const char *text,
    size_t length,
    Lil_Result *result
) {
    memset(result, 0, sizeof(*result));

    if (!source_init(&result->source, path, text, length)) {
        lil_result_fail(result, "out of memory while copying source");
        return false;
    }

    Lexer lexer;
    lexer_init(&lexer, &result->source);

    Parser *parser = parser_create(&lexer);

    if (parser == NULL) {
        lil_result_fail(result, "out of memory while creating parser");
        return false;
    }

    if (!parser_parse_program(parser, &result->program)) {
        result->errors = *parser_diagnostics(parser);
        result->diagnostic = *parser_diagnostic(parser);
        parser_destroy(parser);
        return false;
    }

    parser_destroy(parser);
    result->has_program = true;
    return true;
}

bool lil_analyze_source(
    const char *path,
    const char *text,
    size_t length,
    Lil_Result *result
) {
    if (!lil_run_source(path, text, length, result)) {
        return false;
    }

    if (!sema_analyze(&result->program, &result->source, &result->symbols,
                      &result->errors, &result->warnings)) {
        result->diagnostic = result->errors.items[0];
        program_free(&result->program);
        symbol_table_free(&result->symbols);
        result->has_program = false;
        return false;
    }

    result->is_analyzed = true;
    return true;
}

void lil_result_fprint_diagnostics(FILE *stream, const Lil_Result *result) {
    diagnostic_lists_fprint_merged(stream, &result->source, &result->errors,
                                   &result->warnings);
}

void lil_result_free(Lil_Result *result) {
    program_free(&result->program);
    symbol_table_free(&result->symbols);
    source_free(&result->source);
    result->has_program = false;
    result->is_analyzed = false;
    diagnostic_clear(&result->diagnostic);
    diagnostic_list_clear(&result->errors);
    diagnostic_list_clear(&result->warnings);
}
