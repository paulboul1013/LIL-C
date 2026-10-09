#include "sema.h"

#include "operator.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * See include/sema.h. Recursion depth is bounded by the parser's §6.3
 * limits, like every other recursive pass over the AST.
 */

#define SEMA_BUILTIN_PUTC "putc"
#define SEMA_EXIT_VARIABLE "r0"
/* Names longer than this are shortened to this many bytes plus "..." in
 * messages, so a message always fits in Diagnostic.message. */
#define SEMA_MAX_NAME_IN_MESSAGE 200

typedef struct {
    const Source *source;
    Symbol_Table *symbols;
    Diagnostic_List *errors;
    /* "statement has no effect", in source order. */
    Diagnostic_List no_effect;
    size_t loop_depth;
    /* The error limit was reached or memory ran out: stop analyzing. */
    bool stopped;
} Sema;

static const char *token_text(const Sema *sema, Token token) {
    return source_text_at(sema->source, token.span);
}

static bool is_putc(const Sema *sema, Token token) {
    return symbol_name_equals(token_text(sema, token), token.span.length,
                              SEMA_BUILTIN_PUTC);
}

static int shown_length(size_t length) {
    return length > SEMA_MAX_NAME_IN_MESSAGE ? SEMA_MAX_NAME_IN_MESSAGE
                                             : (int)length;
}

static const char *ellipsis(size_t length) {
    return length > SEMA_MAX_NAME_IN_MESSAGE ? "..." : "";
}

#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
static void sema_error(
    Sema *sema,
    Diagnostic_Stage stage,
    Source_Span span,
    const char *format,
    ...
) {
    if (sema->stopped) {
        return;
    }

    Diagnostic diagnostic;
    diagnostic_clear(&diagnostic);

    va_list args;
    va_start(args, format);
    char message[sizeof(diagnostic.message)];
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    diagnostic_set(&diagnostic, stage, span, "%s", message);
    diagnostic_list_add(sema->errors, &diagnostic);

    if (stage == DIAGNOSTIC_INTERNAL || diagnostic_list_is_full(sema->errors)) {
        sema->stopped = true;
    }
}

static void sema_out_of_memory(Sema *sema, Source_Span span) {
    sema_error(sema, DIAGNOSTIC_INTERNAL, span,
               "out of memory during semantic analysis");
}

static void sema_putc_as_variable(Sema *sema, Source_Span span) {
    sema_error(sema, DIAGNOSTIC_SEMANTIC, span,
               "'putc' is a builtin function and cannot be used as a "
               "variable");
}

/* Resolves the identifier 'name' to a symbol; SYMBOL_NONE on failure. */
static size_t sema_resolve(Sema *sema, Token name) {
    size_t id = SYMBOL_NONE;

    if (!symbol_table_intern(sema->symbols, token_text(sema, name),
                             name.span.length, name.span, &id)) {
        sema_out_of_memory(sema, name.span);
        return SYMBOL_NONE;
    }

    return id;
}

/* Returns true when evaluating 'expression' may have an effect (§3.2):
 * an assignment, a call, or a division (which may trap, §4.6). */
static bool sema_expression(Sema *sema, Expr *expression) {
    if (sema->stopped) {
        return true;
    }

    switch (expression->kind) {
        case EXPR_VARIABLE: {
            const Token name = expression->as.variable;

            if (is_putc(sema, name)) {
                sema_putc_as_variable(sema, name.span);
                return false;
            }

            const size_t id = sema_resolve(sema, name);

            if (id != SYMBOL_NONE) {
                expression->symbol = id;
                ++sema->symbols->symbols[id].read_count;
            }

            return false;
        }

        case EXPR_LITERAL:
            return false;

        case EXPR_UNARY:
            return sema_expression(sema, expression->as.unary.operand);

        case EXPR_BINARY: {
            const bool left = sema_expression(sema, expression->as.binary.left);
            const bool right =
                sema_expression(sema, expression->as.binary.right);
            return left || right ||
                   expression->as.binary.operator_token.kind == TOKEN_SLASH;
        }

        case EXPR_ASSIGNMENT: {
            const Token name = expression->as.assignment.name;

            if (is_putc(sema, name)) {
                sema_putc_as_variable(sema, name.span);
            } else {
                const size_t id = sema_resolve(sema, name);

                if (id != SYMBOL_NONE) {
                    Symbol *symbol = &sema->symbols->symbols[id];

                    expression->symbol = id;

                    if (symbol->write_count == 0) {
                        symbol->first_write_span = name.span;
                    }

                    ++symbol->write_count;
                }
            }

            sema_expression(sema, expression->as.assignment.value);
            return true;
        }

        case EXPR_CALL: {
            const Token name = expression->as.call.name;
            const size_t count = expression->as.call.argument_count;

            if (!is_putc(sema, name)) {
                sema_error(sema, DIAGNOSTIC_SEMANTIC, name.span,
                           "unknown function '%.*s%s'",
                           shown_length(name.span.length),
                           token_text(sema, name),
                           ellipsis(name.span.length));
            } else if (count != 1) {
                sema_error(sema, DIAGNOSTIC_SEMANTIC, name.span,
                           "'putc' expects 1 argument, got %zu", count);
            } else {
                expression->builtin = BUILTIN_PUTC;
            }

            for (size_t i = 0; i < count; ++i) {
                sema_expression(sema, expression->as.call.arguments[i]);
            }

            return true;
        }
    }

    return true;
}

static void sema_statement(Sema *sema, Stmt *statement);

static void sema_statements(Sema *sema, Stmt **statements, size_t count) {
    for (size_t i = 0; i < count && !sema->stopped; ++i) {
        sema_statement(sema, statements[i]);
    }
}

static void sema_loop_body(Sema *sema, Stmt *body) {
    ++sema->loop_depth;
    sema_statement(sema, body);
    --sema->loop_depth;
}

static void sema_statement(Sema *sema, Stmt *statement) {
    if (sema->stopped) {
        return;
    }

    switch (statement->kind) {
        case STMT_EXPRESSION: {
            const size_t errors_before = sema->errors->count;
            const bool effect = sema_expression(sema, statement->as.expression);

            /* An erroneous statement gets its error, not also a warning. */
            if (!effect && sema->errors->count == errors_before) {
                Diagnostic warning;
                diagnostic_clear(&warning);
                diagnostic_set_warning(&warning, DIAGNOSTIC_SEMANTIC,
                                       statement->span,
                                       "statement has no effect");
                diagnostic_list_add(&sema->no_effect, &warning);
            }
            break;
        }

        case STMT_EMPTY:
            break;

        case STMT_BLOCK:
            sema_statements(sema, statement->as.block.statements,
                            statement->as.block.statement_count);
            break;

        case STMT_IF:
            sema_expression(sema, statement->as.if_statement.condition);
            sema_statement(sema, statement->as.if_statement.then_branch);

            if (statement->as.if_statement.else_branch != NULL) {
                sema_statement(sema, statement->as.if_statement.else_branch);
            }
            break;

        case STMT_WHILE:
            sema_expression(sema, statement->as.while_statement.condition);
            sema_loop_body(sema, statement->as.while_statement.body);
            break;

        case STMT_DO:
            sema_loop_body(sema, statement->as.do_statement.body);
            sema_expression(sema, statement->as.do_statement.condition);
            break;

        case STMT_BREAK:
        case STMT_CONTINUE:
            if (sema->loop_depth == 0) {
                sema_error(sema, DIAGNOSTIC_SEMANTIC, statement->span,
                           "'%s' outside of a loop",
                           statement->kind == STMT_BREAK ? "break"
                                                         : "continue");
            }
            break;
    }
}

/*
 * §6.2: merges the no-effect warnings with "assigned but never read"
 * warnings (at each such variable's first assignment) into 'warnings',
 * keeping the first DIAGNOSTIC_MAX_WARNINGS in source order.
 */
static bool sema_finish_warnings(Sema *sema, Diagnostic_List *warnings) {
    const Symbol_Table *symbols = sema->symbols;
    const Symbol **unused = NULL;
    size_t unused_count = 0;

    if (symbols->count > 0) {
        unused = malloc(symbols->count * sizeof(*unused));

        if (unused == NULL) {
            return false;
        }
    }

    /*
     * No sorting is needed: the walk is in source order and interns a
     * name at its first occurrence, so ids follow first_span order. A
     * symbol that is never read first occurs at its first write, so for
     * these symbols id order is first_write_span order. sema_verify()
     * checks both facts.
     */
    for (size_t i = 0; i < symbols->count; ++i) {
        const Symbol *symbol = &symbols->symbols[i];

        if (symbol->write_count > 0 && symbol->read_count == 0 &&
            strcmp(symbol->name, SEMA_EXIT_VARIABLE) != 0) {
            unused[unused_count++] = symbol;
        }
    }

    size_t n = 0;
    size_t u = 0;

    while (!diagnostic_list_is_full(warnings) &&
           (n < sema->no_effect.count || u < unused_count)) {
        if (u >= unused_count ||
            (n < sema->no_effect.count &&
             sema->no_effect.items[n].span.offset <
                 unused[u]->first_write_span.offset)) {
            diagnostic_list_add(warnings, &sema->no_effect.items[n++]);
            continue;
        }

        const Source_Span span = unused[u++]->first_write_span;
        Diagnostic warning;

        diagnostic_clear(&warning);
        diagnostic_set_warning(&warning, DIAGNOSTIC_SEMANTIC, span,
                               "variable '%.*s%s' is assigned but never read",
                               shown_length(span.length),
                               source_text_at(sema->source, span),
                               ellipsis(span.length));
        diagnostic_list_add(warnings, &warning);
    }

    free(unused);
    return true;
}

bool sema_analyze(
    Program *program,
    const Source *source,
    Symbol_Table *symbols,
    Diagnostic_List *errors,
    Diagnostic_List *warnings
) {
    Sema sema;

    memset(&sema, 0, sizeof(sema));
    sema.source = source;
    sema.symbols = symbols;
    sema.errors = errors;

    const size_t errors_before = errors->count;

    sema_statements(&sema, program->statements, program->statement_count);

    /* An incomplete walk has incomplete read counts: no unused-variable
     * warnings then, only the no-effect ones found so far. */
    if (sema.stopped) {
        for (size_t i = 0; i < sema.no_effect.count; ++i) {
            diagnostic_list_add(warnings, &sema.no_effect.items[i]);
        }
    } else if (!sema_finish_warnings(&sema, warnings)) {
        /* At the end of the source, so that it follows every error the
         * walk found and the list stays strictly increasing (§6.1). */
        sema_out_of_memory(&sema, source_end_span(source));
    }

    return errors->count == errors_before;
}

size_t sema_exit_symbol(const Symbol_Table *symbols) {
    return symbol_table_find(symbols, SEMA_EXIT_VARIABLE,
                             strlen(SEMA_EXIT_VARIABLE));
}

/* ---- sema_verify ------------------------------------------------------ */

typedef struct {
    const Source *source;
    const Symbol_Table *symbols;
    size_t loop_depth;
} Verifier;

/* Invariant 1 (and 6: the name is an identifier). SYMBOL_NONE, the
 * parser's "unresolved", is never < count. */
static bool verify_name(const Verifier *verifier, Token name, size_t id) {
    return name.kind == TOKEN_IDENTIFIER && id < verifier->symbols->count &&
           symbol_name_equals(source_text_at(verifier->source, name.span),
                              name.span.length,
                              verifier->symbols->symbols[id].name);
}

/* Invariant 6: numbers are 0..INT64_MAX (§2.7), characters 0..255
 * (§2.6). */
static bool verify_literal(Token literal) {
    switch (literal.kind) {
        case TOKEN_NUMBER:
            return literal.value >= 0;

        case TOKEN_CHARACTER:
            return literal.value >= 0 && literal.value <= 255;

        default:
            return false;
    }
}

static bool verify_expression(const Verifier *verifier, const Expr *expr) {
    if (expr == NULL) {
        return false;
    }

    switch (expr->kind) {
        case EXPR_VARIABLE:
            return verify_name(verifier, expr->as.variable, expr->symbol);

        case EXPR_LITERAL:
            return verify_literal(expr->as.literal);

        case EXPR_UNARY:
            return operator_is_unary(expr->as.unary.operator_token.kind) &&
                   verify_expression(verifier, expr->as.unary.operand);

        case EXPR_BINARY:
            /* '=' is in the §4.2 table but is an EXPR_ASSIGNMENT node. */
            return expr->as.binary.operator_token.kind != TOKEN_ASSIGN &&
                   operator_binary(expr->as.binary.operator_token.kind) !=
                       NULL &&
                   verify_expression(verifier, expr->as.binary.left) &&
                   verify_expression(verifier, expr->as.binary.right);

        case EXPR_ASSIGNMENT:
            return verify_name(verifier, expr->as.assignment.name,
                               expr->symbol) &&
                   verify_expression(verifier, expr->as.assignment.value);

        case EXPR_CALL:
            return expr->builtin == BUILTIN_PUTC &&
                   expr->as.call.argument_count == 1 &&
                   expr->as.call.arguments != NULL &&
                   verify_expression(verifier, expr->as.call.arguments[0]);
    }

    return false;
}

static bool verify_statement(Verifier *verifier, const Stmt *statement);

static bool verify_loop_body(Verifier *verifier, const Stmt *body) {
    ++verifier->loop_depth;
    const bool ok = verify_statement(verifier, body);
    --verifier->loop_depth;
    return ok;
}

static bool verify_statement(Verifier *verifier, const Stmt *statement) {
    if (statement == NULL) {
        return false;
    }

    switch (statement->kind) {
        case STMT_EXPRESSION:
            return verify_expression(verifier, statement->as.expression);

        case STMT_EMPTY:
            return true;

        case STMT_BLOCK:
            if (statement->as.block.statement_count > 0 &&
                statement->as.block.statements == NULL) {
                return false;
            }

            for (size_t i = 0; i < statement->as.block.statement_count; ++i) {
                if (!verify_statement(verifier,
                                      statement->as.block.statements[i])) {
                    return false;
                }
            }
            return true;

        case STMT_IF:
            return verify_expression(verifier,
                                     statement->as.if_statement.condition) &&
                   verify_statement(verifier,
                                    statement->as.if_statement.then_branch) &&
                   (statement->as.if_statement.else_branch == NULL ||
                    verify_statement(verifier,
                                     statement->as.if_statement.else_branch));

        case STMT_WHILE:
            return verify_expression(verifier,
                                     statement->as.while_statement.condition) &&
                   verify_loop_body(verifier,
                                    statement->as.while_statement.body);

        case STMT_DO:
            return verify_loop_body(verifier,
                                    statement->as.do_statement.body) &&
                   verify_expression(verifier,
                                     statement->as.do_statement.condition);

        case STMT_BREAK:
        case STMT_CONTINUE:
            return verifier->loop_depth > 0;
    }

    return false;
}

/*
 * Invariant 5: ids 0..count-1 each name a distinct symbol (the table
 * finds every name at its own id), numbered in order of first appearance
 * (strictly increasing first_span). A symbol that is never read first
 * appears at its first write, which is what lets sema_finish_warnings()
 * emit "assigned but never read" in id order without sorting.
 */
static bool verify_symbols(const Symbol_Table *symbols) {
    for (size_t i = 0; i < symbols->count; ++i) {
        const Symbol *symbol = &symbols->symbols[i];

        if (symbol->name == NULL ||
            strlen(symbol->name) != symbol->name_length ||
            symbol_table_find(symbols, symbol->name, symbol->name_length) !=
                i ||
            symbol->read_count + symbol->write_count == 0) {
            return false;
        }

        if (i > 0 && symbol->first_span.offset <=
                         symbols->symbols[i - 1].first_span.offset) {
            return false;
        }

        if (symbol->write_count > 0 && symbol->read_count == 0 &&
            symbol->first_write_span.offset != symbol->first_span.offset) {
            return false;
        }
    }

    return true;
}

bool sema_verify(
    const Program *program,
    const Source *source,
    const Symbol_Table *symbols
) {
    Verifier verifier = {source, symbols, 0};

    if (symbol_table_find(symbols, SEMA_BUILTIN_PUTC,
                          strlen(SEMA_BUILTIN_PUTC)) != SYMBOL_NONE ||
        !verify_symbols(symbols)) {
        return false;
    }

    if (program->statement_count > 0 && program->statements == NULL) {
        return false;
    }

    for (size_t i = 0; i < program->statement_count; ++i) {
        if (!verify_statement(&verifier, program->statements[i])) {
            return false;
        }
    }

    return true;
}
