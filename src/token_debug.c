#include "token_debug.h"

#include "lexer.h"

void token_debug_print(FILE *stream, const Source *source, Token token) {
    fprintf(
        stream,
        "%s %zu:%zu",
        token_kind_name(token.kind),
        token.span.line,
        token.span.column
    );

    if (token.span.length > 0) {
        const char *text = source_text_at(source, token.span);

        fputc(' ', stream);

        /* §6.4.1: bytes outside printable ASCII are written as \xHH. */
        for (size_t i = 0; i < token.span.length; ++i) {
            const unsigned char byte = (unsigned char)text[i];

            if (byte < 0x20 || byte >= 0x7f) {
                fprintf(stream, "\\x%02x", (unsigned)byte);
            } else {
                fputc(byte, stream);
            }
        }
    }

    fputc('\n', stream);
}

bool token_debug_dump(
    FILE *stream,
    const Source *source,
    Diagnostic *diagnostic
) {
    Lexer lexer;
    lexer_init(&lexer, source);

    for (;;) {
        const Token token = lexer_next_token(&lexer);

        if (token.kind == TOKEN_INVALID) {
            *diagnostic = *lexer_diagnostic(&lexer);
            return false;
        }

        token_debug_print(stream, source, token);

        if (token.kind == TOKEN_END) {
            diagnostic_clear(diagnostic);
            return true;
        }
    }
}
