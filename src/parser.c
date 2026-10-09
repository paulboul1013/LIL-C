#include "parser.h"

#include "operator.h"

#include <stdint.h>
#include <stdlib.h>

struct Parser {
    Lexer *lexer;
    Token current;
    /* Every reported error in source order; items[0] is the first. */
    Diagnostic_List errors;
    /* Open nesting levels (§6.3.2). */
    size_t nesting;
    /* Open blocks; decides what recovery does with '}' (§6.3.4). */
    size_t block_depth;
    /* A lexical error, an internal error or the error cap ends parsing. */
    bool stopped;
    /* The error cap or an internal error: no further error is recorded. */
    bool errors_closed;
    /*
     * The lexical error that stopped parsing (§6.3.4). It is recorded when
     * parsing ends, after the syntax errors found while unwinding that lie
     * before 'lexical_cutoff' (the start of the bad token): a height check
     * (§6.3.3) only runs once the operand, and so the token after it, has
     * been read.
     */
    bool has_lexical_error;
    size_t lexical_cutoff;
    Diagnostic lexical_error;
    /*
     * Abandoned compound statements whose trailing part is still ahead
     * (§6.3.4): if statements that failed before their 'else' could be
     * seen, and do statements whose body failed before 'while (...) ;'.
     * Recovery skips those tails instead of reporting them as new errors.
     */
    size_t abandoned_ifs;
    size_t abandoned_dos;
    /*
     * How many of those (the innermost, which come last) lie beyond the
     * nesting limit (§6.3.2): skipped after an if, while or do statement
     * was refused for going too deep, abandoned at the deepest level, or
     * abandoned while 'quiet_nesting' was set. Their tails are beyond the limit too, so they are parsed
     * with 'quiet_nesting' set: a further "nesting too deep" in them is not
     * reported again (a long 'else if' chain reports once), while an
     * unrelated depth error elsewhere still is. 'deep_failure' says that
     * the last failure was such a refusal.
     */
    size_t deep_ifs;
    size_t deep_dos;
    bool deep_failure;
    bool quiet_nesting;
    /*
     * Parentheses (grouping, call and condition) open at the current token,
     * how many were open where the last error was found, and how many an
     * abandoned statement left unclosed after recovery: a later ')' that
     * would close one is part of that statement, not a new error (§6.3.4).
     */
    size_t paren_depth;
    size_t failure_parens;
    size_t unclosed_parens;
};

/*
 * Records an error unless it would cascade (§6.3.4): nothing is recorded
 * after the error cap or an internal error, nothing at or after a lexical
 * error, and errors stay in strictly increasing source order, so two errors
 * are never reported at the same token. (Within one statement there is
 * never a second error: every parse function returns NULL right after
 * reporting, up to the statement list, which resynchronizes without
 * reporting anything.)
 */
static void parser_add_error(Parser *parser, const Diagnostic *diagnostic) {
    const Diagnostic_List *errors = &parser->errors;

    if (parser->errors_closed) {
        return;
    }

    if (parser->has_lexical_error &&
        diagnostic->span.offset >= parser->lexical_cutoff) {
        return;
    }

    if (errors->count > 0 &&
        errors->items[errors->count - 1].span.offset >=
            diagnostic->span.offset) {
        return;
    }

    diagnostic_list_add(&parser->errors, diagnostic);

    if (diagnostic_list_is_full(&parser->errors)) {
        parser->errors_closed = true;
        parser->stopped = true;
    }
}

/* Remembers where the statement being parsed failed (§6.3.4). */
static void parser_note_failure(Parser *parser) {
    parser->failure_parens = parser->paren_depth;
    parser->deep_failure = false;
}

static void parser_error(Parser *parser, Source_Span span, const char *message) {
    Diagnostic diagnostic;

    parser_note_failure(parser);

    /* A ')' outside every parenthesis of this statement that closes one
     * left open by an abandoned statement belongs to that statement. */
    if (parser->unclosed_parens > 0 && parser->paren_depth == 0 &&
        parser->current.kind == TOKEN_RIGHT_PAREN &&
        span.offset == parser->current.span.offset) {
        return;
    }

    diagnostic_set(&diagnostic, DIAGNOSTIC_SYNTAX, span, "%s", message);
    parser_add_error(parser, &diagnostic);
}

/* Errors that end parsing: running out of memory. */
static void parser_fatal(Parser *parser, Source_Span span, const char *message) {
    Diagnostic diagnostic;
    diagnostic_set(&diagnostic, DIAGNOSTIC_INTERNAL, span, "%s", message);
    parser_add_error(parser, &diagnostic);
    parser->errors_closed = true;
    parser->stopped = true;
}

/* Moves to the next token. A lexical error (even while skipping tokens)
 * stops parsing and is recorded when parsing ends; the current token is
 * then TOKEN_INVALID, which no parse function accepts. */
static void parser_advance(Parser *parser) {
    if (parser->stopped) {
        return;
    }

    parser->current = lexer_next_token(parser->lexer);

    if (parser->current.kind == TOKEN_INVALID) {
        const Diagnostic *lexical = lexer_diagnostic(parser->lexer);

        parser->has_lexical_error = true;
        parser->lexical_error = *lexical;
        parser->lexical_cutoff =
            parser->current.span.offset < lexical->span.offset
                ? parser->current.span.offset
                : lexical->span.offset;
        parser->stopped = true;
    }
}

/* Records the pending lexical error after every syntax error before it. */
static void parser_flush_lexical_error(Parser *parser) {
    if (!parser->has_lexical_error) {
        return;
    }

    parser->has_lexical_error = false;
    parser_add_error(parser, &parser->lexical_error);
}

static bool parser_at(const Parser *parser, Token_Kind kind) {
    return parser->current.kind == kind;
}

/* Reports 'message' at the current token unless it is of 'kind'. Does not
 * consume the token. */
static bool parser_expect(Parser *parser, Token_Kind kind, const char *message) {
    if (parser_at(parser, kind)) {
        return true;
    }

    parser_error(parser, parser->current.span, message);
    return false;
}

Parser *parser_create(Lexer *lexer) {
    Parser *parser = malloc(sizeof(*parser));

    if (parser == NULL) {
        return NULL;
    }

    parser->lexer = lexer;
    parser->nesting = 0;
    parser->block_depth = 0;
    parser->stopped = false;
    parser->errors_closed = false;
    parser->has_lexical_error = false;
    parser->lexical_cutoff = 0;
    diagnostic_clear(&parser->lexical_error);
    parser->abandoned_ifs = 0;
    parser->abandoned_dos = 0;
    parser->deep_ifs = 0;
    parser->deep_dos = 0;
    parser->deep_failure = false;
    parser->quiet_nesting = false;
    parser->paren_depth = 0;
    parser->failure_parens = 0;
    parser->unclosed_parens = 0;
    diagnostic_list_clear(&parser->errors);
    parser_advance(parser);
    return parser;
}

void parser_destroy(Parser *parser) {
    free(parser);
}

static Source_Span source_span_through(
    Source_Span first,
    Source_Span last
) {
    Source_Span span = first;
    span.length = last.offset + last.length - first.offset;
    return span;
}

/*
 * §6.3 nesting limits. Every recursive pass over the AST (freeing,
 * printing, later semantic analysis and lowering) recurses once per tree
 * level, so the parser refuses trees deeper than PARSER_MAX_EXPR_HEIGHT and
 * syntactic nesting deeper than PARSER_MAX_NESTING instead of letting a
 * later pass (or the parser itself) overflow the stack.
 */
static bool parser_check_height(
    Parser *parser,
    size_t height,
    Source_Span span
) {
    if (height <= PARSER_MAX_EXPR_HEIGHT) {
        return true;
    }

    parser_error(parser, span, "nesting too deep");
    return false;
}

/* Opens one nesting level at 'span' (the opening token, which must not be
 * consumed yet so recovery sees it). */
static bool parser_enter_nesting(Parser *parser, Source_Span span) {
    if (parser->nesting >= PARSER_MAX_NESTING) {
        if (parser->quiet_nesting) {
            parser_note_failure(parser);
        } else {
            parser_error(parser, span, "nesting too deep");
        }

        return false;
    }

    ++parser->nesting;
    return true;
}

static void parser_leave_nesting(Parser *parser) {
    --parser->nesting;
}

/* Appends 'item' to a growable pointer array. Geometric growth keeps
 * parsing linear in the number of statements and arguments (§6.3). */
static bool parser_push_stmt(
    Parser *parser,
    Stmt ***items,
    size_t *count,
    size_t *capacity,
    Stmt *item
) {
    if (*count == *capacity) {
        const size_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;
        Stmt **grown = NULL;

        if (new_capacity <= SIZE_MAX / sizeof(**items)) {
            grown = realloc(*items, new_capacity * sizeof(**items));
        }

        if (grown == NULL) {
            parser_fatal(parser, item->span,
                         "out of memory while parsing program");
            return false;
        }

        *items = grown;
        *capacity = new_capacity;
    }

    (*items)[*count] = item;
    ++*count;
    return true;
}

static bool parser_push_expr(
    Parser *parser,
    Expr ***items,
    size_t *count,
    size_t *capacity,
    Expr *item
) {
    if (*count == *capacity) {
        const size_t new_capacity = *capacity == 0 ? 4 : *capacity * 2;
        Expr **grown = NULL;

        if (new_capacity <= SIZE_MAX / sizeof(**items)) {
            grown = realloc(*items, new_capacity * sizeof(**items));
        }

        if (grown == NULL) {
            parser_fatal(parser, item->span,
                         "out of memory while parsing expression");
            return false;
        }

        *items = grown;
        *capacity = new_capacity;
    }

    (*items)[*count] = item;
    ++*count;
    return true;
}

static void expr_array_free(Expr **items, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        expr_free(items[i]);
    }

    free(items);
}

static void stmt_array_free(Stmt **items, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        stmt_free(items[i]);
    }

    free(items);
}

static Expr *parser_new_expr(Parser *parser, Expr_Kind kind, Source_Span span) {
    Expr *expression = calloc(1, sizeof(*expression));

    if (expression == NULL) {
        parser_fatal(parser, span, "out of memory while parsing expression");
        return NULL;
    }

    expression->kind = kind;
    expression->span = span;
    expression->symbol = SYMBOL_NONE;
    return expression;
}

static Stmt *parser_new_stmt(Parser *parser, Stmt_Kind kind, Source_Span span) {
    Stmt *statement = calloc(1, sizeof(*statement));

    if (statement == NULL) {
        parser_fatal(parser, span, "out of memory while parsing statement");
        return NULL;
    }

    statement->kind = kind;
    statement->span = span;
    return statement;
}

/* ---- expressions (§4) ---------------------------------------------------- */

static Expr *parser_parse_expression(
    Parser *parser,
    int min_precedence,
    size_t *height
);
static Expr *parser_parse_prefix(
    Parser *parser,
    size_t *height,
    bool *assignable
);

/* call = identifier "(" [ argument-list ] ")"; 'name' is consumed and the
 * current token is the '('. */
static Expr *parser_parse_call_arguments(
    Parser *parser,
    Token name,
    Token open,
    size_t *height
) {
    Expr **arguments = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t max_height = 0;

    parser_advance(parser);

    if (!parser_at(parser, TOKEN_RIGHT_PAREN)) {
        for (;;) {
            size_t argument_height = 0;
            Expr *argument =
                parser_parse_expression(parser, 0, &argument_height);

            if (argument == NULL) {
                expr_array_free(arguments, count);
                return NULL;
            }

            if (!parser_push_expr(parser, &arguments, &count, &capacity,
                                  argument)) {
                expr_free(argument);
                expr_array_free(arguments, count);
                return NULL;
            }

            if (argument_height > max_height) {
                max_height = argument_height;
            }

            if (!parser_at(parser, TOKEN_COMMA)) {
                break;
            }

            parser_advance(parser);
        }

        if (!parser_expect(parser, TOKEN_RIGHT_PAREN,
                           "expected ')' after arguments")) {
            expr_array_free(arguments, count);
            return NULL;
        }
    }

    const Token close = parser->current;

    if (!parser_check_height(parser, max_height + 1, open.span)) {
        expr_array_free(arguments, count);
        return NULL;
    }

    Expr *call = parser_new_expr(
        parser, EXPR_CALL, source_span_through(name.span, close.span)
    );

    if (call == NULL) {
        expr_array_free(arguments, count);
        return NULL;
    }

    call->as.call.name = name;
    call->as.call.arguments = arguments;
    call->as.call.argument_count = count;
    *height = max_height + 1;
    parser_advance(parser);
    return call;
}

/* "(" expression ")": the parentheses leave no node, but the inner
 * node's span grows to include them (§6.4.3). */
static Expr *parser_parse_parenthesized(Parser *parser, size_t *height) {
    const Token open = parser->current;

    parser_advance(parser);

    Expr *inner = parser_parse_expression(parser, 0, height);

    if (inner == NULL) {
        return NULL;
    }

    if (!parser_expect(parser, TOKEN_RIGHT_PAREN,
                       "expected ')' after expression")) {
        expr_free(inner);
        return NULL;
    }

    inner->span = source_span_through(open.span, parser->current.span);
    parser_advance(parser);
    return inner;
}

/* primary = identifier | integer-literal | character-literal
 *         | "(" expression ")" | call */
static Expr *parser_parse_primary(
    Parser *parser,
    size_t *height,
    bool *assignable
) {
    const Token token = parser->current;

    *assignable = false;

    switch (token.kind) {
        case TOKEN_IDENTIFIER: {
            parser_advance(parser);

            if (parser_at(parser, TOKEN_LEFT_PAREN)) {
                const Token open = parser->current;

                if (!parser_enter_nesting(parser, open.span)) {
                    return NULL;
                }

                ++parser->paren_depth;
                Expr *call =
                    parser_parse_call_arguments(parser, token, open, height);
                --parser->paren_depth;
                parser_leave_nesting(parser);
                return call;
            }

            Expr *variable = parser_new_expr(parser, EXPR_VARIABLE, token.span);

            if (variable != NULL) {
                variable->as.variable = token;
                *height = 1;
                *assignable = true;
            }

            return variable;
        }

        case TOKEN_NUMBER:
        case TOKEN_CHARACTER: {
            Expr *literal = parser_new_expr(parser, EXPR_LITERAL, token.span);

            if (literal != NULL) {
                literal->as.literal = token;
                *height = 1;
                parser_advance(parser);
            }

            return literal;
        }

        case TOKEN_LEFT_PAREN: {
            if (!parser_enter_nesting(parser, token.span)) {
                return NULL;
            }

            ++parser->paren_depth;
            Expr *inner = parser_parse_parenthesized(parser, height);
            --parser->paren_depth;
            parser_leave_nesting(parser);
            return inner;
        }

        default:
            parser_error(parser, token.span, "expected expression");
            return NULL;
    }
}

/* unary = ( "-" | "~" ) unary | primary. 'assignable' tells the caller
 * whether the result is a bare identifier, the only valid target of '='
 * (§4.4). */
static Expr *parser_parse_prefix(
    Parser *parser,
    size_t *height,
    bool *assignable
) {
    if (!operator_is_unary(parser->current.kind)) {
        return parser_parse_primary(parser, height, assignable);
    }

    const Token operator_token = parser->current;
    size_t operand_height = 0;
    bool operand_assignable = false;

    *assignable = false;

    if (!parser_enter_nesting(parser, operator_token.span)) {
        return NULL;
    }

    parser_advance(parser);

    Expr *operand =
        parser_parse_prefix(parser, &operand_height, &operand_assignable);
    parser_leave_nesting(parser);

    if (operand == NULL) {
        return NULL;
    }

    if (!parser_check_height(parser, operand_height + 1,
                             operator_token.span)) {
        expr_free(operand);
        return NULL;
    }

    Expr *unary = parser_new_expr(
        parser,
        EXPR_UNARY,
        source_span_through(operator_token.span, operand->span)
    );

    if (unary == NULL) {
        expr_free(operand);
        return NULL;
    }

    unary->as.unary.operator_token = operator_token;
    unary->as.unary.operand = operand;
    *height = operand_height + 1;
    return unary;
}

static Expr *parser_make_binary(
    Parser *parser,
    Expr *left,
    Token operator_token,
    Expr *right,
    size_t *height,
    size_t right_height
) {
    const size_t new_height =
        1 + (*height > right_height ? *height : right_height);

    if (!parser_check_height(parser, new_height, operator_token.span)) {
        expr_free(left);
        expr_free(right);
        return NULL;
    }

    Expr *expression = parser_new_expr(
        parser, EXPR_BINARY, source_span_through(left->span, right->span)
    );

    if (expression == NULL) {
        expr_free(left);
        expr_free(right);
        return NULL;
    }

    expression->as.binary.left = left;
    expression->as.binary.operator_token = operator_token;
    expression->as.binary.right = right;
    *height = new_height;
    return expression;
}

/* NAME "=" VALUE (§4.4); 'target' is the already parsed left operand and
 * the current token is the '='. */
static Expr *parser_parse_assignment(
    Parser *parser,
    Expr *target,
    bool assignable,
    const Binary_Operator *op,
    size_t *height
) {
    const Token equals_token = parser->current;

    if (!assignable) {
        parser_error(parser, equals_token.span, "invalid assignment target");
        expr_free(target);
        return NULL;
    }

    if (!parser_enter_nesting(parser, equals_token.span)) {
        expr_free(target);
        return NULL;
    }

    parser_advance(parser);

    size_t value_height = 0;
    Expr *value = parser_parse_expression(parser, op->precedence,
                                          &value_height);
    parser_leave_nesting(parser);

    if (value == NULL ||
        !parser_check_height(parser, value_height + 1, equals_token.span)) {
        expr_free(value);
        expr_free(target);
        return NULL;
    }

    const Token name = target->as.variable;

    target->kind = EXPR_ASSIGNMENT;
    target->span = source_span_through(name.span, value->span);
    target->as.assignment.name = name;
    target->as.assignment.equals_token = equals_token;
    target->as.assignment.value = value;
    *height = value_height + 1;
    return target;
}

/*
 * Pratt / precedence climbing over the single operator table (§4.2,
 * src/operator.c): parses an expression whose binary operators all have
 * precedence >= min_precedence. Left-associative chains are built in the
 * loop, so only right-associative operators, parentheses and prefix
 * operators recurse, and those are bounded by the nesting limit.
 */
static Expr *parser_parse_expression(
    Parser *parser,
    int min_precedence,
    size_t *height
) {
    bool assignable = false;
    Expr *left = parser_parse_prefix(parser, height, &assignable);

    while (left != NULL) {
        const Binary_Operator *op = operator_binary(parser->current.kind);

        if (op == NULL || op->precedence < min_precedence) {
            break;
        }

        if (op->kind == TOKEN_ASSIGN) {
            left = parser_parse_assignment(parser, left, assignable, op,
                                           height);
            assignable = false;
            continue;
        }

        /* Every other operator in the table is left-associative: the
         * right operand only takes tighter operators, and the chain is
         * built by this loop without recursing. */
        const Token operator_token = parser->current;
        size_t right_height = 0;

        parser_advance(parser);

        Expr *right = parser_parse_expression(parser, op->precedence + 1,
                                              &right_height);

        if (right == NULL) {
            expr_free(left);
            return NULL;
        }

        left = parser_make_binary(parser, left, operator_token, right,
                                  height, right_height);
        assignable = false;
    }

    return left;
}

/* ---- statements (§3) ----------------------------------------------------- */

static Stmt *parser_parse_statement(Parser *parser);
static Stmt *parser_parse_branch(Parser *parser);
static Expr *parser_parse_condition(Parser *parser, const char *open_message);

/* An if ('kind' TOKEN_IF) or do statement whose tail is still ahead;
 * 'deep' when it lies beyond the nesting limit. */
static void parser_push_abandoned(Parser *parser, Token_Kind kind, bool deep) {
    if (kind == TOKEN_IF) {
        ++parser->abandoned_ifs;
        parser->deep_ifs += deep ? 1 : 0;
    } else {
        ++parser->abandoned_dos;
        parser->deep_dos += deep ? 1 : 0;
    }
}

/* The tail ('kind' TOKEN_ELSE or TOKEN_WHILE) of the innermost pending if
 * or do statement was reached. Returns whether that statement (and so its
 * tail) lies beyond the nesting limit; false when none is pending. */
static bool parser_pop_abandoned(Parser *parser, Token_Kind kind) {
    size_t *pending = &parser->abandoned_ifs;
    size_t *deep = &parser->deep_ifs;

    if (kind == TOKEN_WHILE) {
        pending = &parser->abandoned_dos;
        deep = &parser->deep_dos;
    }

    if (*pending == 0) {
        return false;
    }

    --*pending;

    if (*deep == 0) {
        return false;
    }

    /* Deep ones are the innermost, which an 'else' or 'while' meets first. */
    --*deep;
    return true;
}

/*
 * §6.3.4 recovery: skips tokens up to and including the next ';' outside
 * the braces opened while skipping, or a whole skipped block; stops before
 * a '}' that closes the enclosing block (consumes a stray one at top
 * level), at end of file, or when parsing stopped. Outside those braces an
 * 'else' or 'while' skipped is the tail of an abandoned if or do statement
 * (when one is pending), which then no longer has a tail to skip; an 'if'
 * or 'do' skipped is abandoned too and may still have a tail ahead. The
 * parentheses still open after the skipped tokens are remembered in
 * 'unclosed_parens'.
 */
static void parser_synchronize(Parser *parser) {
    size_t depth = 0;
    size_t parens = parser->unclosed_parens + parser->failure_parens;
    /* An if or do skipped here lies inside a statement refused for going
     * too deep, or inside a tail already beyond the limit. */
    const bool deep = parser->deep_failure || parser->quiet_nesting;

    parser->failure_parens = 0;
    parser->deep_failure = false;

    while (!parser->stopped && !parser_at(parser, TOKEN_END)) {
        const Token_Kind kind = parser->current.kind;

        if (kind == TOKEN_LEFT_BRACE) {
            parens = 0;
            ++depth;
        } else if (kind == TOKEN_RIGHT_BRACE) {
            parens = 0;

            if (depth == 0 && parser->block_depth > 0) {
                break;
            }

            if (depth <= 1) {
                parser_advance(parser);
                break;
            }

            --depth;
        } else if (depth > 0) {
            /* Inside a skipped block: nothing else matters. */
        } else if (kind == TOKEN_SEMICOLON) {
            parser_advance(parser);
            break;
        } else if (kind == TOKEN_LEFT_PAREN) {
            ++parens;
        } else if (kind == TOKEN_RIGHT_PAREN) {
            if (parens > 0) {
                --parens;
            }
        } else if (kind == TOKEN_IF || kind == TOKEN_DO) {
            parser_push_abandoned(parser, kind, deep);
        } else if (kind == TOKEN_ELSE || kind == TOKEN_WHILE) {
            parser_pop_abandoned(parser, kind);
        }

        parser_advance(parser);
    }

    parser->unclosed_parens = parens;
}

/* Records that a statement starting with `kind` was abandoned before its
 * trailing part ('else ...' or 'while (...) ;') was reached. */
static void parser_note_abandoned(Parser *parser, Token_Kind kind) {
    /* At the deepest level its tail ('else' statement or the condition
     * after 'while') would open a level beyond the limit. */
    const bool deep = parser->quiet_nesting ||
                      parser->nesting >= PARSER_MAX_NESTING;

    parser_push_abandoned(parser, kind, deep);
}

/* Skips the 'while' "(" expression ")" ";" tail of an abandoned do
 * statement. Errors inside it are real and are reported. */
static bool parser_skip_do_tail(Parser *parser) {
    parser_advance(parser);

    Expr *condition =
        parser_parse_condition(parser, "expected '(' after 'while'");

    if (condition == NULL) {
        return false;
    }

    expr_free(condition);

    if (!parser_expect(parser, TOKEN_SEMICOLON,
                       "expected ';' after 'do' statement")) {
        return false;
    }

    parser_advance(parser);
    return true;
}

/*
 * §6.3.4 after resynchronizing: an 'else' that belongs to an abandoned if
 * statement, or the 'while (...) ;' of an abandoned do statement, is part
 * of the statement that already failed, not a new mistake. It is skipped
 * without reporting it; the statement after 'else' is still parsed (and
 * discarded) so real errors inside it are reported.
 */
static void parser_skip_abandoned_tails(Parser *parser) {
    const bool outer_quiet = parser->quiet_nesting;

    /* With a parenthesis still open the abandoned statement goes on: its
     * tails can only follow the ')' that closes it. */
    if (parser->unclosed_parens > 0) {
        return;
    }

    while (!parser->stopped) {
        /* The tail of a statement beyond the nesting limit is beyond it as
         * well (§6.3.2): the limit is reported once for all of them. This
         * is decided for every tail, so a chain that first goes too deep
         * inside an earlier tail is still reported once. Recovery after a
         * failed tail runs in the same mode. */
        if (parser_at(parser, TOKEN_ELSE) && parser->abandoned_ifs > 0) {
            parser->quiet_nesting =
                parser_pop_abandoned(parser, TOKEN_ELSE) || outer_quiet;
            parser_advance(parser);

            Stmt *skipped = parser_parse_branch(parser);

            if (skipped == NULL) {
                parser_synchronize(parser);
            } else {
                stmt_free(skipped);
            }
        } else if (parser_at(parser, TOKEN_WHILE) &&
                   parser->abandoned_dos > 0) {
            parser->quiet_nesting =
                parser_pop_abandoned(parser, TOKEN_WHILE) || outer_quiet;

            if (!parser_skip_do_tail(parser)) {
                parser_synchronize(parser);
            }
        } else {
            break;
        }
    }

    parser->abandoned_ifs = 0;
    parser->abandoned_dos = 0;
    parser->deep_ifs = 0;
    parser->deep_dos = 0;
    parser->quiet_nesting = outer_quiet;
}

/* Parses statements up to (not including) a '}' or end of file. Failed
 * statements are dropped after resynchronizing. */
static bool parser_parse_statement_list(
    Parser *parser,
    Stmt ***statements,
    size_t *count
) {
    size_t capacity = 0;
    /* Tails pending in an enclosing list stay pending across this one. */
    const size_t outer_ifs = parser->abandoned_ifs;
    const size_t outer_dos = parser->abandoned_dos;
    const size_t outer_deep_ifs = parser->deep_ifs;
    const size_t outer_deep_dos = parser->deep_dos;
    /* Parentheses never span braces: the block starts with none left open
     * by an abandoned statement, and those left open outside it are still
     * open after it (§6.3.4 resets them only when a brace is skipped). */
    const size_t outer_parens = parser->unclosed_parens;

    *statements = NULL;
    *count = 0;
    parser->abandoned_ifs = 0;
    parser->abandoned_dos = 0;
    parser->deep_ifs = 0;
    parser->deep_dos = 0;
    parser->unclosed_parens = 0;

    while (!parser->stopped && !parser_at(parser, TOKEN_END)) {
        if (parser_at(parser, TOKEN_RIGHT_BRACE)) {
            if (parser->block_depth > 0) {
                break;
            }

            /* §3.1 a '}' outside every block. */
            parser_error(parser, parser->current.span, "unexpected '}'");
            parser_advance(parser);

            /* §6.3.4 parentheses and tails never cross a brace. */
            parser->unclosed_parens = 0;
            parser->abandoned_ifs = 0;
            parser->abandoned_dos = 0;
            parser->deep_ifs = 0;
            parser->deep_dos = 0;
            continue;
        }

        Stmt *statement = parser_parse_statement(parser);

        if (statement == NULL) {
            parser_synchronize(parser);
            parser_skip_abandoned_tails(parser);
            continue;
        }

        /* A complete statement is not part of an abandoned one: it closes
         * no parenthesis and is followed by no tail left open before. */
        parser->unclosed_parens = 0;
        parser->abandoned_ifs = 0;
        parser->abandoned_dos = 0;
        parser->deep_ifs = 0;
        parser->deep_dos = 0;

        if (!parser_push_stmt(parser, statements, count, &capacity,
                              statement)) {
            stmt_free(statement);
            break;
        }
    }

    parser->abandoned_ifs = outer_ifs;
    parser->abandoned_dos = outer_dos;
    parser->deep_ifs = outer_deep_ifs;
    parser->deep_dos = outer_deep_dos;
    parser->unclosed_parens = outer_parens;
    return !parser->stopped;
}

/* block = "{" { statement } "}" */
static Stmt *parser_parse_block(Parser *parser) {
    const Token open = parser->current;
    Stmt **statements = NULL;
    size_t count = 0;

    parser_advance(parser);
    ++parser->block_depth;
    parser_parse_statement_list(parser, &statements, &count);
    --parser->block_depth;

    if (!parser_expect(parser, TOKEN_RIGHT_BRACE,
                       "expected '}' before end of file")) {
        stmt_array_free(statements, count);
        return NULL;
    }

    Stmt *block = parser_new_stmt(
        parser, STMT_BLOCK, source_span_through(open.span, parser->current.span)
    );

    if (block == NULL) {
        stmt_array_free(statements, count);
        return NULL;
    }

    block->as.block.statements = statements;
    block->as.block.statement_count = count;
    parser_advance(parser);
    return block;
}

/* "(" expression ")" after 'if' or 'while'. */
static Expr *parser_parse_condition(Parser *parser, const char *open_message) {
    if (!parser_expect(parser, TOKEN_LEFT_PAREN, open_message)) {
        return NULL;
    }

    ++parser->paren_depth;
    parser_advance(parser);

    size_t height = 0;
    Expr *condition = parser_parse_expression(parser, 0, &height);

    if (condition != NULL &&
        !parser_expect(parser, TOKEN_RIGHT_PAREN,
                       "expected ')' after condition")) {
        expr_free(condition);
        condition = NULL;
    }

    --parser->paren_depth;

    if (condition != NULL) {
        parser_advance(parser);
    }

    return condition;
}

/* The statement of an if/else branch or a loop body. A token that can
 * never start a statement there ('else', '}' or end of file) is reported
 * as a missing statement. */
static Stmt *parser_parse_branch(Parser *parser) {
    if (parser_at(parser, TOKEN_ELSE) ||
        parser_at(parser, TOKEN_RIGHT_BRACE) ||
        parser_at(parser, TOKEN_END)) {
        parser_error(parser, parser->current.span, "expected statement");
        return NULL;
    }

    return parser_parse_statement(parser);
}

/* if-statement = "if" "(" expression ")" statement [ "else" statement ];
 * 'else' binds to the nearest 'if' (§3.4) because the innermost if
 * statement takes it first. */
static Stmt *parser_parse_if(Parser *parser) {
    const Token keyword = parser->current;

    parser_advance(parser);

    Expr *condition = parser_parse_condition(parser, "expected '(' after 'if'");

    if (condition == NULL) {
        parser_note_abandoned(parser, TOKEN_IF);
        return NULL;
    }

    Stmt *then_branch = parser_parse_branch(parser);

    if (then_branch == NULL) {
        parser_note_abandoned(parser, TOKEN_IF);
        expr_free(condition);
        return NULL;
    }

    Stmt *else_branch = NULL;

    if (parser_at(parser, TOKEN_ELSE)) {
        parser_advance(parser);
        else_branch = parser_parse_branch(parser);

        if (else_branch == NULL) {
            expr_free(condition);
            stmt_free(then_branch);
            return NULL;
        }
    }

    const Stmt *last = else_branch != NULL ? else_branch : then_branch;
    Stmt *statement = parser_new_stmt(
        parser, STMT_IF, source_span_through(keyword.span, last->span)
    );

    if (statement == NULL) {
        expr_free(condition);
        stmt_free(then_branch);
        stmt_free(else_branch);
        return NULL;
    }

    statement->as.if_statement.condition = condition;
    statement->as.if_statement.then_branch = then_branch;
    statement->as.if_statement.else_branch = else_branch;
    return statement;
}

/* while-statement = "while" "(" expression ")" statement */
static Stmt *parser_parse_while(Parser *parser) {
    const Token keyword = parser->current;

    parser_advance(parser);

    Expr *condition =
        parser_parse_condition(parser, "expected '(' after 'while'");

    if (condition == NULL) {
        return NULL;
    }

    Stmt *body = parser_parse_branch(parser);

    if (body == NULL) {
        expr_free(condition);
        return NULL;
    }

    Stmt *statement = parser_new_stmt(
        parser, STMT_WHILE, source_span_through(keyword.span, body->span)
    );

    if (statement == NULL) {
        expr_free(condition);
        stmt_free(body);
        return NULL;
    }

    statement->as.while_statement.condition = condition;
    statement->as.while_statement.body = body;
    return statement;
}

/* do-statement = "do" statement "while" "(" expression ")" ";" */
static Stmt *parser_parse_do(Parser *parser) {
    const Token keyword = parser->current;

    parser_advance(parser);

    Stmt *body = parser_parse_branch(parser);

    if (body == NULL) {
        parser_note_abandoned(parser, TOKEN_DO);
        return NULL;
    }

    if (!parser_expect(parser, TOKEN_WHILE,
                       "expected 'while' after 'do' body")) {
        stmt_free(body);
        return NULL;
    }

    parser_advance(parser);

    Expr *condition =
        parser_parse_condition(parser, "expected '(' after 'while'");

    if (condition == NULL ||
        !parser_expect(parser, TOKEN_SEMICOLON,
                       "expected ';' after 'do' statement")) {
        expr_free(condition);
        stmt_free(body);
        return NULL;
    }

    Stmt *statement = parser_new_stmt(
        parser, STMT_DO, source_span_through(keyword.span, parser->current.span)
    );

    if (statement == NULL) {
        expr_free(condition);
        stmt_free(body);
        return NULL;
    }

    statement->as.do_statement.body = body;
    statement->as.do_statement.condition = condition;
    parser_advance(parser);
    return statement;
}

/* break-statement / continue-statement / empty-statement: KEYWORD? ";" */
static Stmt *parser_parse_simple(
    Parser *parser,
    Stmt_Kind kind,
    const char *message
) {
    const Token first = parser->current;

    if (kind != STMT_EMPTY) {
        parser_advance(parser);

        if (!parser_expect(parser, TOKEN_SEMICOLON, message)) {
            return NULL;
        }
    }

    Stmt *statement = parser_new_stmt(
        parser, kind, source_span_through(first.span, parser->current.span)
    );

    if (statement != NULL) {
        parser_advance(parser);
    }

    return statement;
}

/* expression-statement = expression ";" */
static Stmt *parser_parse_expression_statement(Parser *parser) {
    size_t height = 0;
    Expr *expression = parser_parse_expression(parser, 0, &height);

    if (expression == NULL) {
        return NULL;
    }

    if (!parser_expect(parser, TOKEN_SEMICOLON,
                       "expected ';' after expression")) {
        expr_free(expression);
        return NULL;
    }

    Stmt *statement = parser_new_stmt(
        parser,
        STMT_EXPRESSION,
        source_span_through(expression->span, parser->current.span)
    );

    if (statement == NULL) {
        expr_free(expression);
        return NULL;
    }

    statement->as.expression = expression;
    parser_advance(parser);
    return statement;
}

/* Statements that open a nesting level (§6.3.2): blocks and control
 * statements. The opening token is checked before it is consumed. */
static Stmt *parser_parse_nested_statement(
    Parser *parser,
    Stmt *(*parse)(Parser *parser)
) {
    if (!parser_enter_nesting(parser, parser->current.span)) {
        /* Recovery skips the refused statement from its first token on,
         * counting it (when it is an if or do) as abandoned beyond the
         * limit, together with every if and do inside it. */
        parser->deep_failure = true;
        return NULL;
    }

    Stmt *statement = parse(parser);
    parser_leave_nesting(parser);
    return statement;
}

static Stmt *parser_parse_statement(Parser *parser) {
    switch (parser->current.kind) {
        case TOKEN_SEMICOLON:
            return parser_parse_simple(parser, STMT_EMPTY, NULL);

        case TOKEN_LEFT_BRACE:
            return parser_parse_nested_statement(parser, parser_parse_block);

        case TOKEN_IF:
            return parser_parse_nested_statement(parser, parser_parse_if);

        case TOKEN_WHILE:
            return parser_parse_nested_statement(parser, parser_parse_while);

        case TOKEN_DO:
            return parser_parse_nested_statement(parser, parser_parse_do);

        case TOKEN_BREAK:
            return parser_parse_simple(parser, STMT_BREAK,
                                       "expected ';' after 'break'");

        case TOKEN_CONTINUE:
            return parser_parse_simple(parser, STMT_CONTINUE,
                                       "expected ';' after 'continue'");

        case TOKEN_ELSE:
            parser_error(parser, parser->current.span,
                         "'else' without a matching 'if'");
            return NULL;

        default:
            return parser_parse_expression_statement(parser);
    }
}

bool parser_parse_program(Parser *parser, Program *out) {
    const Source *source = parser->lexer->source;

    out->span = (Source_Span){0, source->length, 1, 1};
    out->statements = NULL;
    out->statement_count = 0;

    Stmt **statements = NULL;
    size_t count = 0;

    parser_parse_statement_list(parser, &statements, &count);
    parser_flush_lexical_error(parser);

    if (parser->stopped || parser->errors.count > 0) {
        stmt_array_free(statements, count);
        return false;
    }

    out->statements = statements;
    out->statement_count = count;
    return true;
}

const Diagnostic *parser_diagnostic(const Parser *parser) {
    /* items[0] is all zero (no error) until the first error is added. */
    return &parser->errors.items[0];
}

const Diagnostic_List *parser_diagnostics(const Parser *parser) {
    return &parser->errors;
}
