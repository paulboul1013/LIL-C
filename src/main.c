#include "lexer.h"
#include "token.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>

static char *read_file(
    const char *path,
    size_t *out_length
)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    const long file_size = ftell(file);

    if (file_size < 0) {
        fclose(file);
        return NULL;
    }

    rewind(file);

    char *buffer =
        malloc((size_t)file_size + 1);

    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    const size_t bytes_read = fread(
        buffer,
        1,
        (size_t)file_size,
        file
    );

    if (
        bytes_read != (size_t)file_size &&
        ferror(file)
    ) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[bytes_read] = '\0';

    fclose(file);

    *out_length = bytes_read;
    return buffer;
}

static void print_token(const Token *token)
{
    printf(
        "%zu:%zu  %-18s  \"",
        token->line,
        token->column,
        token_kind_name(token->kind)
    );

    fwrite(
        token->start,
        1,
        token->length,
        stdout
    );

    puts("\"");
}


int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(
            stderr,
            "usage: %s <source.lil>\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    size_t source_length = 0;

    char *source = read_file(
        argv[1],
        &source_length
    );

    if (source == NULL) {
        fprintf(
            stderr,
            "failed to read file: %s\n",
            argv[1]
        );

        return EXIT_FAILURE;
    }

    Lexer lexer;

    lexer_init(
        &lexer,
        source,
        source_length
    );

    Parser parser;
    
    if (!parser_init(&parser,&lexer)) {
        fprintf(
            stderr,
            "%s:%s\n",
            argv[1],
            parser_error_message(&parser)
        );

        free(source);
        return EXIT_FAILURE;
    }

    Flat_Expression expression;

    if (!parser_parse_flat_expression(&parser,&expression)) {
        fprintf(
            stderr,
            "%s:%s\n",
            argv[1],
            parser_error_message(&parser)
        );

        flat_expression_free(&expression);
        free(source);

        return EXIT_FAILURE;
    }



    flat_expression_print_ascii(&expression);

    flat_expression_free(&expression);

    free(source);
    return EXIT_SUCCESS;
}