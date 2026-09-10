#include "lil.h"
#include "ast_debug.h"

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

    Lil_Result result;
    const bool succeeded = lil_run_source(
        argv[1],
        source,
        source_length,
        &result
    );
    free(source);

    if (!succeeded) {
        diagnostic_fprint(stderr, &result.source, &result.diagnostic);
        lil_result_free(&result);
        return EXIT_FAILURE;
    }

    ast_debug_print_program(
        stdout,
        &result.source,
        &result.program
    );
    lil_result_free(&result);
    return EXIT_SUCCESS;
}
