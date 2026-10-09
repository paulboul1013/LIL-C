#include "ir_lower.h"

#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * See include/ir_lower.h and docs/spec.md §8.5. Recursion depth is
 * bounded by the parser's §6.3 limits, like every other pass over the
 * AST.
 *
 * Blocks are created when they first become a jump target, but laid out
 * in the order they are *started* (made current), so the dump reads top
 * to bottom like the source: a loop's condition, its body (with every
 * nested construct), then the code after it.
 */

typedef struct {
    size_t break_target;
    size_t continue_target;
} Loop;

typedef struct {
    Ir_Program *ir;
    size_t current;             /* block receiving instructions */
    size_t construct_count;     /* numbers the labels: then.1, ... */
    size_t *order;              /* block indices in the order started */
    size_t order_count;
    size_t order_capacity;
    bool failed;                /* out of memory */
} Lower;

/* A fresh block named PREFIX.NUMBER (or just PREFIX when number is 0). */
static size_t new_block(Lower *lower, const char *prefix, size_t number,
                        Source_Span span) {
    char name[64];
    const int length = number == 0
        ? snprintf(name, sizeof(name), "%s", prefix)
        : snprintf(name, sizeof(name), "%s.%zu", prefix, number);
    size_t index = 0;

    if (lower->failed ||
        !ir_program_add_block(lower->ir, name, (size_t)length, span,
                              &index)) {
        lower->failed = true;
        return 0;
    }

    return index;
}

/* Makes 'block' current and records its layout position. */
static void start_block(Lower *lower, size_t block) {
    if (lower->failed) {
        return;
    }

    if (lower->order_count == lower->order_capacity) {
        const size_t capacity =
            lower->order_capacity == 0 ? 16 : lower->order_capacity * 2;
        size_t *grown = capacity > SIZE_MAX / sizeof(size_t)
            ? NULL
            : realloc(lower->order, capacity * sizeof(size_t));

        if (grown == NULL) {
            lower->failed = true;
            return;
        }

        lower->order = grown;
        lower->order_capacity = capacity;
    }

    lower->order[lower->order_count++] = block;
    lower->current = block;
}

static void emit(Lower *lower, Ir_Instruction instruction) {
    if (lower->failed ||
        !ir_block_append(&lower->ir->blocks[lower->current], &instruction)) {
        lower->failed = true;
    }
}

static size_t new_vreg(Lower *lower) {
    return ir_program_new_vreg(lower->ir);
}

static size_t emit_const(Lower *lower, int64_t value, Source_Span span) {
    const size_t dest = new_vreg(lower);
    emit(lower, ir_make_const(dest, value, span));
    return dest;
}

static size_t lower_expression(Lower *lower, const Expr *expr);

static Ir_Opcode binary_opcode(Token_Kind kind) {
    switch (kind) {
        case TOKEN_PLUS: return IR_ADD;
        case TOKEN_MINUS: return IR_SUB;
        case TOKEN_STAR: return IR_MUL;
        case TOKEN_SLASH: return IR_DIV;
        case TOKEN_AMPERSAND: return IR_AND;
        case TOKEN_PIPE: return IR_OR;
        case TOKEN_EQUAL_EQUAL: return IR_EQ;
        case TOKEN_NOT_EQUAL: return IR_NE;
        case TOKEN_LESS: return IR_LT;
        case TOKEN_LESS_EQUAL: return IR_LE;
        case TOKEN_GREATER: return IR_GT;
        default: return IR_GE;          /* TOKEN_GREATER_EQUAL */
    }
}

/*
 * §4.9. For 'a && b':
 *
 *       %l = <a>
 *       %r = const 0
 *       br %l, and.rhs.N, and.end.N
 *     and.rhs.N:
 *       %b = <b>
 *       %z = const 0
 *       %r = ne %b, %z
 *       jmp and.end.N
 *     and.end.N:
 *
 * and 'a || b' is the same with const 1 and the branch targets swapped.
 * %r is the one vreg lowering assigns twice; it is defined on both paths
 * into the end block.
 */
static size_t lower_short_circuit(Lower *lower, const Expr *expr,
                                  bool is_and) {
    const Source_Span span = expr->span;
    const size_t left = lower_expression(lower, expr->as.binary.left);
    const size_t number = ++lower->construct_count;
    const size_t rhs = new_block(lower, is_and ? "and.rhs" : "or.rhs",
                                 number, span);
    const size_t end = new_block(lower, is_and ? "and.end" : "or.end",
                                 number, span);
    const size_t result = new_vreg(lower);

    emit(lower, ir_make_const(result, is_and ? 0 : 1, span));
    emit(lower, is_and ? ir_make_br(left, rhs, end, span)
                       : ir_make_br(left, end, rhs, span));

    start_block(lower, rhs);
    const size_t right = lower_expression(lower, expr->as.binary.right);
    const size_t zero = emit_const(lower, 0, span);
    emit(lower, ir_make_binary(IR_NE, result, right, zero, span));
    emit(lower, ir_make_jmp(end, span));

    start_block(lower, end);
    return result;
}

static size_t lower_expression(Lower *lower, const Expr *expr) {
    const Source_Span span = expr->span;

    switch (expr->kind) {
        case EXPR_VARIABLE: {
            const size_t dest = new_vreg(lower);
            emit(lower, ir_make_load(dest, expr->symbol, span));
            return dest;
        }

        case EXPR_LITERAL:
            return emit_const(lower, expr->as.literal.value, span);

        case EXPR_UNARY: {
            const size_t operand =
                lower_expression(lower, expr->as.unary.operand);
            const size_t dest = new_vreg(lower);
            const Ir_Opcode opcode =
                expr->as.unary.operator_token.kind == TOKEN_MINUS ? IR_NEG
                                                                  : IR_NOT;
            emit(lower, ir_make_unary(opcode, dest, operand, span));
            return dest;
        }

        case EXPR_BINARY: {
            const Token_Kind kind = expr->as.binary.operator_token.kind;

            if (kind == TOKEN_LOGICAL_AND || kind == TOKEN_LOGICAL_OR) {
                return lower_short_circuit(lower, expr,
                                           kind == TOKEN_LOGICAL_AND);
            }

            /* §4.3: the left operand is evaluated completely first. */
            const size_t left = lower_expression(lower, expr->as.binary.left);
            const size_t right =
                lower_expression(lower, expr->as.binary.right);
            const size_t dest = new_vreg(lower);
            emit(lower,
                 ir_make_binary(binary_opcode(kind), dest, left, right, span));
            return dest;
        }

        case EXPR_ASSIGNMENT: {
            /* §4.4: the value first, then the store; the result is the
             * stored value. */
            const size_t value =
                lower_expression(lower, expr->as.assignment.value);
            emit(lower, ir_make_store(expr->symbol, value, span));
            return value;
        }

        case EXPR_CALL:
            break;
    }

    /* EXPR_CALL: always putc with one argument (post-sema invariant 3);
     * its value is the argument itself (§4.10). */
    const size_t value = lower_expression(lower, expr->as.call.arguments[0]);
    emit(lower, ir_make_putc(value, span));
    return value;
}

static void lower_statement(Lower *lower, const Stmt *statement,
                            const Loop *loop);

/* break/continue: the jump, then an unreachable block for whatever
 * follows in the same statement list (§8.5). */
static void lower_jump_out(Lower *lower, size_t target, Source_Span span) {
    emit(lower, ir_make_jmp(target, span));

    const size_t dead = new_block(lower, "dead", ++lower->construct_count,
                                  span);
    start_block(lower, dead);
}

static void lower_if(Lower *lower, const Stmt *statement, const Loop *loop) {
    const Source_Span span = statement->span;
    const size_t condition =
        lower_expression(lower, statement->as.if_statement.condition);
    const size_t number = ++lower->construct_count;
    const size_t then_block = new_block(lower, "if.then", number, span);
    const bool has_else = statement->as.if_statement.else_branch != NULL;
    const size_t else_block =
        has_else ? new_block(lower, "if.else", number, span) : 0;
    const size_t end = new_block(lower, "if.end", number, span);

    emit(lower, ir_make_br(condition, then_block,
                           has_else ? else_block : end, span));

    start_block(lower, then_block);
    lower_statement(lower, statement->as.if_statement.then_branch, loop);
    emit(lower, ir_make_jmp(end, span));

    if (has_else) {
        start_block(lower, else_block);
        lower_statement(lower, statement->as.if_statement.else_branch, loop);
        emit(lower, ir_make_jmp(end, span));
    }

    start_block(lower, end);
}

static void lower_while(Lower *lower, const Stmt *statement) {
    const Source_Span span = statement->span;
    const size_t number = ++lower->construct_count;
    const size_t cond = new_block(lower, "while.cond", number, span);
    const size_t body = new_block(lower, "while.body", number, span);
    const size_t end = new_block(lower, "while.end", number, span);
    const Loop loop = {end, cond};

    emit(lower, ir_make_jmp(cond, span));

    start_block(lower, cond);
    const size_t condition =
        lower_expression(lower, statement->as.while_statement.condition);
    emit(lower, ir_make_br(condition, body, end, span));

    start_block(lower, body);
    lower_statement(lower, statement->as.while_statement.body, &loop);
    emit(lower, ir_make_jmp(cond, span));

    start_block(lower, end);
}

static void lower_do(Lower *lower, const Stmt *statement) {
    const Source_Span span = statement->span;
    const size_t number = ++lower->construct_count;
    const size_t body = new_block(lower, "do.body", number, span);
    const size_t cond = new_block(lower, "do.cond", number, span);
    const size_t end = new_block(lower, "do.end", number, span);
    /* §3.7: 'continue' in a do loop evaluates the condition next. */
    const Loop loop = {end, cond};

    emit(lower, ir_make_jmp(body, span));

    start_block(lower, body);
    lower_statement(lower, statement->as.do_statement.body, &loop);
    emit(lower, ir_make_jmp(cond, span));

    start_block(lower, cond);
    const size_t condition =
        lower_expression(lower, statement->as.do_statement.condition);
    emit(lower, ir_make_br(condition, body, end, span));

    start_block(lower, end);
}

static void lower_statement(Lower *lower, const Stmt *statement,
                            const Loop *loop) {
    switch (statement->kind) {
        case STMT_EXPRESSION:
            (void)lower_expression(lower, statement->as.expression);
            break;

        case STMT_EMPTY:
            break;

        case STMT_BLOCK:
            for (size_t i = 0; i < statement->as.block.statement_count; ++i) {
                lower_statement(lower, statement->as.block.statements[i],
                                loop);
            }
            break;

        case STMT_IF:
            lower_if(lower, statement, loop);
            break;

        case STMT_WHILE:
            lower_while(lower, statement);
            break;

        case STMT_DO:
            lower_do(lower, statement);
            break;

        /* Post-sema invariant 4: 'loop' is non-NULL here. */
        case STMT_BREAK:
            lower_jump_out(lower, loop->break_target, statement->span);
            break;

        case STMT_CONTINUE:
            lower_jump_out(lower, loop->continue_target, statement->span);
            break;
    }
}

/* Lays the blocks out in the order they were started (every block is
 * started exactly once) and renumbers the jump targets to match. */
static bool reorder_blocks(Lower *lower) {
    Ir_Program *ir = lower->ir;
    const size_t count = ir->block_count;
    Ir_Block *blocks = malloc(count * sizeof(Ir_Block));
    size_t *position = malloc(count * sizeof(size_t));

    if (blocks == NULL || position == NULL) {
        free(blocks);
        free(position);
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        blocks[i] = ir->blocks[lower->order[i]];
        position[lower->order[i]] = i;
    }

    for (size_t i = 0; i < count; ++i) {
        for (size_t k = 0; k < blocks[i].count; ++k) {
            Ir_Instruction *instruction = &blocks[i].instructions[k];

            for (size_t t = 0; t < ir_opcode_target_count(instruction->opcode);
                 ++t) {
                instruction->targets[t] = position[instruction->targets[t]];
            }
        }
    }

    free(ir->blocks);
    free(position);
    ir->blocks = blocks;
    ir->block_capacity = count;
    return true;
}

bool ir_lower_program(
    const Program *program,
    const Symbol_Table *symbols,
    Ir_Program *out
) {
    Lower lower;

    memset(&lower, 0, sizeof(lower));
    ir_program_init(out);
    lower.ir = out;

    for (size_t i = 0; i < symbols->count && !lower.failed; ++i) {
        size_t id = 0;

        lower.failed = !ir_program_add_global(
            out, symbols->symbols[i].name, symbols->symbols[i].name_length,
            &id);
    }

    start_block(&lower, new_block(&lower, "entry", 0, program->span));

    for (size_t i = 0; i < program->statement_count; ++i) {
        lower_statement(&lower, program->statements[i], NULL);
    }

    /* §5.5: exit with r0 (0 when the program never mentions r0). */
    const Source_Span end_span = program->span;
    const size_t exit_symbol = sema_exit_symbol(symbols);
    size_t exit_value;

    if (exit_symbol == SYMBOL_NONE) {
        exit_value = emit_const(&lower, 0, end_span);
    } else {
        exit_value = new_vreg(&lower);
        emit(&lower, ir_make_load(exit_value, exit_symbol, end_span));
    }

    emit(&lower, ir_make_exit(exit_value, end_span));

    if (lower.failed || !reorder_blocks(&lower)) {
        free(lower.order);
        ir_program_free(out);
        return false;
    }

    free(lower.order);
    return true;
}
