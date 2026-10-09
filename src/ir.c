#include "ir.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* See include/ir.h and docs/spec.md §8. */

typedef struct {
    const char *name;
    Ir_Shape shape;
} Ir_Opcode_Info;

/* Indexed by Ir_Opcode. */
static const Ir_Opcode_Info opcode_info[IR_OPCODE_COUNT] = {
    [IR_CONST] = {"const", IR_SHAPE_CONST},
    [IR_COPY] = {"copy", IR_SHAPE_UNARY},
    [IR_ADD] = {"add", IR_SHAPE_BINARY},
    [IR_SUB] = {"sub", IR_SHAPE_BINARY},
    [IR_MUL] = {"mul", IR_SHAPE_BINARY},
    [IR_DIV] = {"div", IR_SHAPE_BINARY},
    [IR_AND] = {"and", IR_SHAPE_BINARY},
    [IR_OR] = {"or", IR_SHAPE_BINARY},
    [IR_NEG] = {"neg", IR_SHAPE_UNARY},
    [IR_NOT] = {"not", IR_SHAPE_UNARY},
    [IR_EQ] = {"eq", IR_SHAPE_BINARY},
    [IR_NE] = {"ne", IR_SHAPE_BINARY},
    [IR_LT] = {"lt", IR_SHAPE_BINARY},
    [IR_LE] = {"le", IR_SHAPE_BINARY},
    [IR_GT] = {"gt", IR_SHAPE_BINARY},
    [IR_GE] = {"ge", IR_SHAPE_BINARY},
    [IR_LOAD] = {"load", IR_SHAPE_LOAD},
    [IR_STORE] = {"store", IR_SHAPE_STORE},
    [IR_PUTC] = {"putc", IR_SHAPE_USE},
    [IR_JMP] = {"jmp", IR_SHAPE_JMP},
    [IR_BR] = {"br", IR_SHAPE_BR},
    [IR_EXIT] = {"exit", IR_SHAPE_USE},
};

static bool opcode_is_valid(Ir_Opcode opcode) {
    return (int)opcode >= 0 && (int)opcode < IR_OPCODE_COUNT;
}

const char *ir_opcode_name(Ir_Opcode opcode) {
    return opcode_is_valid(opcode) ? opcode_info[opcode].name : "?";
}

bool ir_opcode_from_name(const char *name, size_t length, Ir_Opcode *opcode) {
    for (int i = 0; i < IR_OPCODE_COUNT; ++i) {
        const char *candidate = opcode_info[i].name;

        if (strlen(candidate) == length &&
            memcmp(candidate, name, length) == 0) {
            *opcode = (Ir_Opcode)i;
            return true;
        }
    }

    return false;
}

Ir_Shape ir_opcode_shape(Ir_Opcode opcode) {
    return opcode_is_valid(opcode) ? opcode_info[opcode].shape
                                   : IR_SHAPE_USE;
}

bool ir_opcode_is_terminator(Ir_Opcode opcode) {
    return opcode == IR_JMP || opcode == IR_BR || opcode == IR_EXIT;
}

bool ir_opcode_has_dest(Ir_Opcode opcode) {
    switch (ir_opcode_shape(opcode)) {
        case IR_SHAPE_CONST:
        case IR_SHAPE_UNARY:
        case IR_SHAPE_BINARY:
        case IR_SHAPE_LOAD:
            return true;
        default:
            return false;
    }
}

size_t ir_opcode_operand_count(Ir_Opcode opcode) {
    switch (ir_opcode_shape(opcode)) {
        case IR_SHAPE_BINARY:
            return 2;
        case IR_SHAPE_UNARY:
        case IR_SHAPE_STORE:
        case IR_SHAPE_USE:
        case IR_SHAPE_BR:
            return 1;
        default:
            return 0;
    }
}

size_t ir_opcode_target_count(Ir_Opcode opcode) {
    return opcode == IR_BR ? 2 : opcode == IR_JMP ? 1 : 0;
}

static Ir_Instruction make(Ir_Opcode opcode, Source_Span span) {
    Ir_Instruction instruction;

    memset(&instruction, 0, sizeof(instruction));
    instruction.opcode = opcode;
    instruction.dest = IR_NONE;
    instruction.operands[0] = IR_NONE;
    instruction.operands[1] = IR_NONE;
    instruction.global = IR_NONE;
    instruction.targets[0] = IR_NONE;
    instruction.targets[1] = IR_NONE;
    instruction.span = span;
    return instruction;
}

Ir_Instruction ir_make_const(size_t dest, int64_t value, Source_Span span) {
    Ir_Instruction instruction = make(IR_CONST, span);
    instruction.dest = dest;
    instruction.value = value;
    return instruction;
}

Ir_Instruction ir_make_unary(Ir_Opcode opcode, size_t dest, size_t a,
                             Source_Span span) {
    Ir_Instruction instruction = make(opcode, span);
    instruction.dest = dest;
    instruction.operands[0] = a;
    return instruction;
}

Ir_Instruction ir_make_binary(Ir_Opcode opcode, size_t dest, size_t a,
                              size_t b, Source_Span span) {
    Ir_Instruction instruction = make(opcode, span);
    instruction.dest = dest;
    instruction.operands[0] = a;
    instruction.operands[1] = b;
    return instruction;
}

Ir_Instruction ir_make_load(size_t dest, size_t global, Source_Span span) {
    Ir_Instruction instruction = make(IR_LOAD, span);
    instruction.dest = dest;
    instruction.global = global;
    return instruction;
}

Ir_Instruction ir_make_store(size_t global, size_t a, Source_Span span) {
    Ir_Instruction instruction = make(IR_STORE, span);
    instruction.global = global;
    instruction.operands[0] = a;
    return instruction;
}

Ir_Instruction ir_make_putc(size_t a, Source_Span span) {
    Ir_Instruction instruction = make(IR_PUTC, span);
    instruction.operands[0] = a;
    return instruction;
}

Ir_Instruction ir_make_jmp(size_t target, Source_Span span) {
    Ir_Instruction instruction = make(IR_JMP, span);
    instruction.targets[0] = target;
    return instruction;
}

Ir_Instruction ir_make_br(size_t condition, size_t if_true, size_t if_false,
                          Source_Span span) {
    Ir_Instruction instruction = make(IR_BR, span);
    instruction.operands[0] = condition;
    instruction.targets[0] = if_true;
    instruction.targets[1] = if_false;
    return instruction;
}

Ir_Instruction ir_make_exit(size_t a, Source_Span span) {
    Ir_Instruction instruction = make(IR_EXIT, span);
    instruction.operands[0] = a;
    return instruction;
}

void ir_program_init(Ir_Program *program) {
    memset(program, 0, sizeof(*program));
}

void ir_program_free(Ir_Program *program) {
    for (size_t i = 0; i < program->global_count; ++i) {
        free(program->globals[i].name);
    }

    for (size_t i = 0; i < program->block_count; ++i) {
        free(program->blocks[i].name);
        free(program->blocks[i].instructions);
    }

    free(program->globals);
    free(program->blocks);
    ir_program_init(program);
}

/* Grows *items (of item_size bytes each) to hold at least count + 1. */
static bool reserve(void **items, size_t *capacity, size_t count,
                    size_t item_size) {
    if (count < *capacity) {
        return true;
    }

    const size_t new_capacity = *capacity == 0 ? 8 : *capacity * 2;

    if (new_capacity < *capacity || new_capacity > SIZE_MAX / item_size) {
        return false;
    }

    void *grown = realloc(*items, new_capacity * item_size);

    if (grown == NULL) {
        return false;
    }

    *items = grown;
    *capacity = new_capacity;
    return true;
}

static char *copy_name(const char *name, size_t length) {
    char *copy = malloc(length + 1);

    if (copy != NULL) {
        memcpy(copy, name, length);
        copy[length] = '\0';
    }

    return copy;
}

bool ir_program_add_global(Ir_Program *program, const char *name,
                           size_t length, size_t *id) {
    void *items = program->globals;

    if (!reserve(&items, &program->global_capacity, program->global_count,
                 sizeof(Ir_Global))) {
        return false;
    }

    program->globals = items;

    char *copy = copy_name(name, length);

    if (copy == NULL) {
        return false;
    }

    program->globals[program->global_count].name = copy;
    *id = program->global_count++;
    return true;
}

bool ir_program_add_block(Ir_Program *program, const char *name,
                          size_t length, Source_Span span, size_t *index) {
    void *items = program->blocks;

    if (!reserve(&items, &program->block_capacity, program->block_count,
                 sizeof(Ir_Block))) {
        return false;
    }

    program->blocks = items;

    char *copy = copy_name(name, length);

    if (copy == NULL) {
        return false;
    }

    Ir_Block *block = &program->blocks[program->block_count];
    memset(block, 0, sizeof(*block));
    block->name = copy;
    block->span = span;
    *index = program->block_count++;
    return true;
}

size_t ir_program_new_vreg(Ir_Program *program) {
    return program->vreg_count++;
}

bool ir_block_append(Ir_Block *block, const Ir_Instruction *instruction) {
    void *items = block->instructions;

    if (!reserve(&items, &block->capacity, block->count,
                 sizeof(Ir_Instruction))) {
        return false;
    }

    block->instructions = items;
    block->instructions[block->count++] = *instruction;
    return true;
}

const Ir_Instruction *ir_block_terminator(const Ir_Block *block) {
    if (block->count == 0 ||
        !ir_opcode_is_terminator(block->instructions[block->count - 1].opcode)) {
        return NULL;
    }

    return &block->instructions[block->count - 1];
}

static bool name_equals(const char *name, const char *spelling,
                        size_t length) {
    return strlen(name) == length && memcmp(name, spelling, length) == 0;
}

size_t ir_program_find_block(const Ir_Program *program, const char *name,
                             size_t length) {
    for (size_t i = 0; i < program->block_count; ++i) {
        if (name_equals(program->blocks[i].name, name, length)) {
            return i;
        }
    }

    return IR_NONE;
}

size_t ir_program_find_global(const Ir_Program *program, const char *name,
                              size_t length) {
    for (size_t i = 0; i < program->global_count; ++i) {
        if (name_equals(program->globals[i].name, name, length)) {
            return i;
        }
    }

    return IR_NONE;
}

/* ---- dump (§8.3) ------------------------------------------------------- */

static void dump_vreg(FILE *stream, size_t vreg) {
    if (vreg == IR_NONE) {
        fputs("%?", stream);
    } else {
        fprintf(stream, "%%%zu", vreg);
    }
}

static void dump_global(FILE *stream, const Ir_Program *program,
                        size_t global) {
    if (global < program->global_count) {
        fprintf(stream, "@%s", program->globals[global].name);
    } else {
        fputs("@?", stream);
    }
}

static void dump_target(FILE *stream, const Ir_Program *program,
                        size_t target) {
    fputs(target < program->block_count ? program->blocks[target].name : "?",
          stream);
}

static void dump_instruction(FILE *stream, const Ir_Program *program,
                             const Ir_Instruction *instruction) {
    fputs("  ", stream);

    if (ir_opcode_has_dest(instruction->opcode)) {
        dump_vreg(stream, instruction->dest);
        fputs(" = ", stream);
    }

    fputs(ir_opcode_name(instruction->opcode), stream);
    fputc(' ', stream);

    switch (ir_opcode_shape(instruction->opcode)) {
        case IR_SHAPE_CONST:
            fprintf(stream, "%" PRId64, instruction->value);
            break;

        case IR_SHAPE_UNARY:
        case IR_SHAPE_USE:
            dump_vreg(stream, instruction->operands[0]);
            break;

        case IR_SHAPE_BINARY:
            dump_vreg(stream, instruction->operands[0]);
            fputs(", ", stream);
            dump_vreg(stream, instruction->operands[1]);
            break;

        case IR_SHAPE_LOAD:
            dump_global(stream, program, instruction->global);
            break;

        case IR_SHAPE_STORE:
            dump_global(stream, program, instruction->global);
            fputs(", ", stream);
            dump_vreg(stream, instruction->operands[0]);
            break;

        case IR_SHAPE_JMP:
            dump_target(stream, program, instruction->targets[0]);
            break;

        case IR_SHAPE_BR:
            dump_vreg(stream, instruction->operands[0]);
            fputs(", ", stream);
            dump_target(stream, program, instruction->targets[0]);
            fputs(", ", stream);
            dump_target(stream, program, instruction->targets[1]);
            break;
    }

    fputc('\n', stream);
}

void ir_program_dump(FILE *stream, const Ir_Program *program) {
    for (size_t i = 0; i < program->global_count; ++i) {
        fprintf(stream, "global @%s\n", program->globals[i].name);
    }

    for (size_t i = 0; i < program->block_count; ++i) {
        const Ir_Block *block = &program->blocks[i];

        if (i > 0 || program->global_count > 0) {
            fputc('\n', stream);
        }

        fprintf(stream, "%s:\n", block->name);

        for (size_t k = 0; k < block->count; ++k) {
            dump_instruction(stream, program, &block->instructions[k]);
        }
    }
}
