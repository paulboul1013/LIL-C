#include "lil.h"
#include "ast_debug.h"
#include "ir.h"
#include "ir_interp.h"
#include "ir_lower.h"
#include "ir_opt.h"
#include "ir_parse.h"
#include "ir_verify.h"
#include "token_debug.h"

#include <errno.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads the whole file in growing chunks until EOF. The size is never
 * taken from fseek/ftell: that fails on pipes and returns nonsense for
 * directories (LONG_MAX with glibc), which used to turn into a huge
 * allocation. A directory instead makes fread fail, which is reported as
 * an unreadable file (spec §6.4). */
static char *read_file(
    const char *path,
    size_t *out_length
)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return NULL;
    }

    size_t capacity = 4096;
    size_t length = 0;
    char *buffer = malloc(capacity);

    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }

    for (;;) {
        /* Keep one byte free for the terminating NUL. */
        if (capacity - length < 2) {
            if (capacity > SIZE_MAX / 2) {
                free(buffer);
                fclose(file);
                return NULL;
            }

            char *grown = realloc(buffer, capacity * 2);

            if (grown == NULL) {
                free(buffer);
                fclose(file);
                return NULL;
            }

            buffer = grown;
            capacity *= 2;
        }

        const size_t wanted = capacity - length - 1;
        const size_t bytes_read = fread(
            buffer + length,
            1,
            wanted,
            file
        );
        length += bytes_read;

        if (bytes_read < wanted) {
            break;
        }
    }

    if (ferror(file)) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[length] = '\0';

    fclose(file);

    *out_length = length;
    return buffer;
}

/* The last path component of argv[0], so usage text does not depend on
 * the directory the compiler was started from. */
static const char *program_name(int argc, char **argv)
{
    if (argc < 1 || argv[0] == NULL || argv[0][0] == '\0') {
        return "lilc";
    }

    const char *name = strrchr(argv[0], '/');
    return name != NULL && name[1] != '\0' ? name + 1 : argv[0];
}

typedef enum {
    DUMP_NONE,
    DUMP_TOKENS,
    DUMP_AST,
    DUMP_IR,
    DUMP_ASM,
    /* --interp is a mode like the dumps: run the IR interpreter. */
    DUMP_INTERP
} Dump_Mode;

/* Most entries a --passes list may have. */
#define MAX_PASSES 64

typedef struct {
    const char *source_path;
    const char *output_path;   /* NULL unless -o was given */
    Dump_Mode dump;
    const char *dump_flag;     /* spelling of the --dump-* option */
    int optimization_level;    /* 0 or 1 */
    bool ir_input;             /* --ir-input: the source is IR text */
    bool step_limit_given;
    uint64_t step_limit;       /* --step-limit (0 = none) */
    /* --passes=LIST: run exactly these IR passes instead of -O0/-O1. */
    bool passes_given;
    size_t pass_count;
    Ir_Pass passes[MAX_PASSES];
} Options;

static const struct {
    const char *flag;
    Dump_Mode mode;
} dump_flags[] = {
    {"--dump-tokens", DUMP_TOKENS},
    {"--dump-ast", DUMP_AST},
    {"--dump-ir", DUMP_IR},
    {"--dump-asm", DUMP_ASM},
    {"--interp", DUMP_INTERP},
};

static void print_usage(const char *name)
{
    fprintf(
        stderr,
        "usage: %s <source.lil> [-o <file>] "
        "[--dump-tokens|--dump-ast|--dump-ir|--dump-asm|--interp] "
        "[--ir-input] [--step-limit <n>] [-O0|-O1|--passes=<list>]\n",
        name
    );
}

/* Spec §6.4.2: a command-line error followed by the usage line. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static bool usage_error(const char *name, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    print_usage(name);
    return false;
}

/* A decimal step count for --step-limit (digits only; 0 = no limit). */
static bool parse_step_limit(const char *text, uint64_t *limit)
{
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || *end != '\0') {
        return false;
    }

    *limit = (uint64_t)value;
    return true;
}

/* --passes=LIST (spec §6.4): comma-separated pass names (§8.7). */
static bool parse_passes(const char *name, const char *list,
                         Options *options)
{
    options->pass_count = 0;
    options->passes_given = true;

    for (const char *item = list;;) {
        const char *comma = strchr(item, ',');
        const size_t length = comma == NULL ? strlen(item)
                                            : (size_t)(comma - item);
        Ir_Pass pass;

        if (!ir_pass_from_name(item, length, &pass)) {
            return usage_error(name, "unknown pass: '%.*s'", (int)length,
                               item);
        }

        if (options->pass_count == MAX_PASSES) {
            return usage_error(name, "too many passes (the limit is %d)",
                               MAX_PASSES);
        }

        options->passes[options->pass_count++] = pass;

        if (comma == NULL) {
            return true;
        }

        item = comma + 1;
    }
}

/* Parses the command line (spec §6.4); reports errors and returns false
 * when it is not valid. */
static bool parse_options(int argc, char **argv, Options *options)
{
    const char *name = program_name(argc, argv);

    options->source_path = NULL;
    options->output_path = NULL;
    options->dump = DUMP_NONE;
    options->dump_flag = NULL;
    options->optimization_level = 0;
    options->ir_input = false;
    options->step_limit_given = false;
    options->step_limit = IR_INTERP_DEFAULT_STEP_LIMIT;
    options->passes_given = false;
    options->pass_count = 0;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (arg[0] != '-' || arg[1] == '\0') {
            if (options->source_path != NULL) {
                return usage_error(name, "more than one source file: %s", arg);
            }

            options->source_path = arg;
            continue;
        }

        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                return usage_error(name, "missing file name after -o");
            }

            if (options->output_path != NULL) {
                return usage_error(name, "-o given more than once");
            }

            options->output_path = argv[++i];
            continue;
        }

        if (strcmp(arg, "--ir-input") == 0) {
            options->ir_input = true;
            continue;
        }

        if (strcmp(arg, "--step-limit") == 0) {
            if (i + 1 >= argc) {
                return usage_error(name, "missing number after --step-limit");
            }

            if (!parse_step_limit(argv[++i], &options->step_limit)) {
                return usage_error(name, "invalid step limit: %s", argv[i]);
            }

            options->step_limit_given = true;
            continue;
        }

        if (strncmp(arg, "--passes=", 9) == 0) {
            if (!parse_passes(name, arg + 9, options)) {
                return false;
            }

            continue;
        }

        if (strcmp(arg, "-O0") == 0 || strcmp(arg, "-O1") == 0) {
            options->optimization_level = arg[2] - '0';
            continue;
        }

        bool is_dump = false;

        for (size_t k = 0; k < sizeof(dump_flags) / sizeof(dump_flags[0]);
             ++k) {
            if (strcmp(arg, dump_flags[k].flag) != 0) {
                continue;
            }

            if (options->dump != DUMP_NONE &&
                options->dump != dump_flags[k].mode) {
                return usage_error(
                    name,
                    "conflicting options: %s and %s",
                    options->dump_flag,
                    arg
                );
            }

            options->dump = dump_flags[k].mode;
            options->dump_flag = dump_flags[k].flag;
            is_dump = true;
            break;
        }

        if (!is_dump) {
            return usage_error(name, "unknown option: %s", arg);
        }
    }

    if (options->source_path == NULL) {
        print_usage(name);
        return false;
    }

    if (options->output_path != NULL && options->dump != DUMP_NONE) {
        return usage_error(
            name, "-o cannot be combined with %s", options->dump_flag
        );
    }

    if (options->ir_input &&
        (options->dump == DUMP_TOKENS || options->dump == DUMP_AST)) {
        return usage_error(
            name, "--ir-input cannot be combined with %s", options->dump_flag
        );
    }

    if (options->step_limit_given && options->dump != DUMP_INTERP) {
        return usage_error(name, "--step-limit requires --interp");
    }

    if (options->passes_given && options->dump != DUMP_IR &&
        options->dump != DUMP_INTERP) {
        return usage_error(name, "--passes requires --dump-ir or --interp");
    }

    return true;
}

/* Spec §6.4.2: output that could not be written (for example a full
 * disk) is an error, so a dump never exits 0 with truncated output. */
static int finish_stdout(int status)
{
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "failed to write output\n");
        return EXIT_FAILURE;
    }

    return status;
}

/* --dump-tokens (spec §6.4.1): the lexer only, no parsing. */
static int dump_tokens(const char *path, const char *text, size_t length)
{
    Source source;

    if (!source_init(&source, path, text, length)) {
        fprintf(stderr, "%s: internal error: out of memory\n", path);
        return EXIT_FAILURE;
    }

    Diagnostic diagnostic;
    const bool succeeded = token_debug_dump(stdout, &source, &diagnostic);

    if (!succeeded) {
        /* Keep the order readable when stdout and stderr share a tty. */
        fflush(stdout);
        diagnostic_fprint(stderr, &source, &diagnostic);
    }

    source_free(&source);
    return finish_stdout(succeeded ? EXIT_SUCCESS : EXIT_FAILURE);
}

/* Prints an error that is not tied to a source position. */
static int fail_internal(const char *path, const char *message)
{
    fprintf(stderr, "%s: internal error: %s\n", path, message);
    return EXIT_FAILURE;
}

/* run_ir() returns this when the interpreted program ended (exit or trap):
 * the caller releases everything, then calls ir_interp_terminate() with
 * interp_result, so the process ends like the native program would. */
#define STATUS_TERMINATE (-1)
static Ir_Interp_Result interp_result;

/*
 * The IR stages (spec §6.4, §8): optimize (-O1 or --passes), verify, then dump or
 * interpret. 'program' is valid (ir_verify() succeeded) and is released
 * here; 'source' only names the file in diagnostics.
 */
static int run_ir(const Options *options, const Source *source,
                  Ir_Program *program)
{
    Diagnostic error;

    const bool optimized =
        options->passes_given
            ? ir_optimize_passes(program, options->passes,
                                 options->pass_count, NULL, &error)
            : ir_optimize(program, options->optimization_level, &error);

    if (!optimized) {
        diagnostic_fprint(stderr, source, &error);
        ir_program_free(program);
        return EXIT_FAILURE;
    }

    /* §8.7: IR_OPT_MAX_ROUNDS was hit; the program is still correct. */
    if (diagnostic_is_warning(&error)) {
        fprintf(stderr, "%s: internal warning: %s\n", source->path,
                error.message);
    }

    if (!ir_verify(program, &error)) {
        fprintf(stderr, "%s: internal error: IR verification failed after "
                "optimization: %s\n", source->path, error.message);
        ir_program_free(program);
        return EXIT_FAILURE;
    }

    if (options->dump == DUMP_IR) {
        ir_program_dump(stdout, program);
        ir_program_free(program);
        return finish_stdout(EXIT_SUCCESS);
    }

    if (options->dump != DUMP_INTERP) {
        ir_program_free(program);

        if (options->dump == DUMP_ASM) {
            fprintf(stderr, "%s is not implemented yet\n", options->dump_flag);
        } else {
            /* -o takes effect once the back end exists. */
            fprintf(stderr,
                    "generating an executable is not implemented yet\n");
        }

        return EXIT_FAILURE;
    }

    /* Spec §8.6: putc output goes to stdout, the status is r0 & 0xff,
     * a trap is death by SIGFPE. */
    ir_interp_run(program, stdout, options->step_limit, &interp_result);
    ir_program_free(program);

    switch (interp_result.status) {
        case IR_INTERP_EXITED:
        case IR_INTERP_TRAPPED:
            return STATUS_TERMINATE;

        case IR_INTERP_STEP_LIMIT:
            fflush(stdout);
            fprintf(stderr,
                    "interpreter step limit of %" PRIu64 " exceeded\n",
                    options->step_limit);
            return EXIT_FAILURE;

        case IR_INTERP_OUT_OF_MEMORY:
            break;
    }

    return fail_internal(source->path, "out of memory");
}

/* Ends the process: ir_interp_terminate() for STATUS_TERMINATE (after
 * everything was released), otherwise returns the status from main. */
static int finish(int status)
{
    if (status == STATUS_TERMINATE) {
        ir_interp_terminate(&interp_result);
    }

    return status;
}

/* --ir-input: the source file is IR text (spec §8.3). */
static int run_ir_input(const Options *options, const char *text,
                        size_t length)
{
    Source source;

    if (!source_init(&source, options->source_path, text, length)) {
        return fail_internal(options->source_path, "out of memory");
    }

    Ir_Program program;
    Diagnostic error;

    if (!ir_parse(&source, &program, &error) || !ir_verify(&program, &error)) {
        diagnostic_fprint(stderr, &source, &error);
        ir_program_free(&program);
        source_free(&source);
        return EXIT_FAILURE;
    }

    const int status = run_ir(options, &source, &program);
    source_free(&source);
    return status;
}

int main(int argc, char **argv)
{
    Options options;

    if (!parse_options(argc, argv, &options)) {
        return EXIT_FAILURE;
    }

    size_t source_length = 0;

    char *source = read_file(
        options.source_path,
        &source_length
    );

    if (source == NULL) {
        fprintf(
            stderr,
            "failed to read file: %s\n",
            options.source_path
        );

        return EXIT_FAILURE;
    }

    if (options.dump == DUMP_TOKENS) {
        const int status = dump_tokens(
            options.source_path,
            source,
            source_length
        );
        free(source);
        return status;
    }

    if (options.ir_input) {
        const int status = run_ir_input(&options, source, source_length);
        free(source);
        return finish(status);
    }

    Lil_Result result;
    const bool succeeded = lil_analyze_source(
        options.source_path,
        source,
        source_length,
        &result
    );
    free(source);

    /* Every error and warning in source order (§6.1, §6.2, §6.3.4). */
    lil_result_fprint_diagnostics(stderr, &result);

    if (!succeeded) {
        lil_result_free(&result);
        return EXIT_FAILURE;
    }

    if (options.dump == DUMP_AST) {
        ast_debug_print_program(stdout, &result.source, &result.program);
        lil_result_free(&result);
        return finish_stdout(EXIT_SUCCESS);
    }

    /* Spec §8.5: lower, then verify the lowering (always: it is linear
     * for lowered IR, and a failure is a compiler bug). */
    Ir_Program program;
    Diagnostic error;

    if (!ir_lower_program(&result.program, &result.symbols, &program)) {
        const int status = fail_internal(result.source.path, "out of memory");
        lil_result_free(&result);
        return status;
    }

    if (!ir_verify(&program, &error)) {
        fprintf(stderr, "%s: internal error: IR verification failed after "
                "lowering: %s\n", result.source.path, error.message);
        ir_program_free(&program);
        lil_result_free(&result);
        return EXIT_FAILURE;
    }

    const int status = run_ir(&options, &result.source, &program);
    lil_result_free(&result);
    return finish(status);
}
