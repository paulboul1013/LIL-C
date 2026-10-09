#define _POSIX_C_SOURCE 200809L

#include "ir_interp.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

/*
 * See include/ir_interp.h and docs/spec.md §8.6. Arithmetic is done on
 * uint64_t, where wraparound is defined; converting back to int64_t goes
 * through to_signed(), so the interpreter itself never overflows a
 * signed integer (§4.5).
 */

/* The int64_t with the same two's-complement bits as 'bits', without the
 * implementation-defined unsigned -> signed conversion. */
static int64_t to_signed(uint64_t bits) {
    if (bits <= (uint64_t)INT64_MAX) {
        return (int64_t)bits;
    }

    /* bits - 2^64 = -(2^64 - bits) = -(~bits + 1) = -(int64)~bits - 1. */
    return -(int64_t)~bits - 1;
}

bool ir_eval_binary(Ir_Opcode opcode, int64_t a, int64_t b, int64_t *result) {
    const uint64_t ua = (uint64_t)a;
    const uint64_t ub = (uint64_t)b;

    switch (opcode) {
        case IR_ADD: *result = to_signed(ua + ub); return true;
        case IR_SUB: *result = to_signed(ua - ub); return true;
        case IR_MUL: *result = to_signed(ua * ub); return true;
        case IR_AND: *result = to_signed(ua & ub); return true;
        case IR_OR: *result = to_signed(ua | ub); return true;
        case IR_EQ: *result = a == b; return true;
        case IR_NE: *result = a != b; return true;
        case IR_LT: *result = a < b; return true;
        case IR_LE: *result = a <= b; return true;
        case IR_GT: *result = a > b; return true;
        case IR_GE: *result = a >= b; return true;
        default: break;
    }

    /* IR_DIV (§4.6): x86-64 idiv traps on both of these. */
    if (b == 0 || (a == INT64_MIN && b == -1)) {
        return false;
    }

    *result = a / b;    /* C truncates toward zero, like idiv */
    return true;
}

int64_t ir_eval_unary(Ir_Opcode opcode, int64_t a) {
    switch (opcode) {
        case IR_NEG: return to_signed(0u - (uint64_t)a);
        case IR_NOT: return to_signed(~(uint64_t)a);
        default: return a;      /* IR_COPY */
    }
}

void ir_interp_run(
    const Ir_Program *program,
    FILE *output,
    uint64_t step_limit,
    Ir_Interp_Result *result
) {
    memset(result, 0, sizeof(*result));

    int64_t *globals = calloc(program->global_count + 1, sizeof(int64_t));
    int64_t *vregs = calloc(program->vreg_count + 1, sizeof(int64_t));

    if (globals == NULL || vregs == NULL) {
        free(globals);
        free(vregs);
        result->status = IR_INTERP_OUT_OF_MEMORY;
        return;
    }

    const Ir_Block *block = &program->blocks[0];
    size_t index = 0;

    for (;;) {
        if (step_limit != 0 && result->steps == step_limit) {
            result->status = IR_INTERP_STEP_LIMIT;
            break;
        }

        const Ir_Instruction *instruction = &block->instructions[index++];
        const size_t *operands = instruction->operands;

        ++result->steps;

        switch (instruction->opcode) {
            case IR_CONST:
                vregs[instruction->dest] = instruction->value;
                continue;

            case IR_COPY:
            case IR_NEG:
            case IR_NOT:
                vregs[instruction->dest] =
                    ir_eval_unary(instruction->opcode, vregs[operands[0]]);
                continue;

            case IR_LOAD:
                vregs[instruction->dest] = globals[instruction->global];
                continue;

            case IR_STORE:
                globals[instruction->global] = vregs[operands[0]];
                continue;

            case IR_PUTC:
                /* §4.10: the low byte; write errors are ignored. */
                (void)fputc((int)((uint64_t)vregs[operands[0]] & 0xffu),
                            output);
                continue;

            case IR_JMP:
                block = &program->blocks[instruction->targets[0]];
                index = 0;
                continue;

            case IR_BR:
                block = &program->blocks[
                    instruction->targets[vregs[operands[0]] != 0 ? 0 : 1]];
                index = 0;
                continue;

            case IR_EXIT:
                result->status = IR_INTERP_EXITED;
                result->exit_value = vregs[operands[0]];
                result->exit_code =
                    (int)((uint64_t)result->exit_value & 0xffu);
                break;

            default:
                /* Binary and compare opcodes. */
                if (ir_eval_binary(instruction->opcode, vregs[operands[0]],
                                   vregs[operands[1]],
                                   &vregs[instruction->dest])) {
                    continue;
                }

                result->status = IR_INTERP_TRAPPED;
                result->trap_span = instruction->span;
                break;
        }

        break;
    }

    free(globals);
    free(vregs);
}

void ir_interp_restore_sigfpe(void) {
    struct sigaction action;
    sigset_t set;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGFPE, &action, NULL);
    sigemptyset(&set);
    sigaddset(&set, SIGFPE);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

void ir_interp_terminate(const Ir_Interp_Result *result) {
    fflush(stdout);

    if (result->status == IR_INTERP_TRAPPED) {
        ir_interp_restore_sigfpe();
        raise(SIGFPE);
        /* Not reached: the default action of SIGFPE ends the process. */
        abort();
    }

    exit(result->exit_code);
}
