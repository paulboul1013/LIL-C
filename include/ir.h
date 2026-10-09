#ifndef LIL_IR_H
#define LIL_IR_H

#include "source.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * The LIL-C intermediate representation (docs/spec.md §8).
 *
 * A program is a list of globals and a list of basic blocks. Blocks[0] is
 * the entry. Each block is a straight-line list of three-address
 * instructions whose LAST instruction is its only terminator (jmp, br or
 * exit); the control-flow graph is given by those terminators' targets
 * (block indices).
 *
 * Values. Every value is a 64-bit two's-complement integer. Virtual
 * registers (%0, %1, ...) are unlimited in number and are NOT SSA: a vreg
 * may be assigned more than once (lowering does this only for the result
 * of '&&' and '||', which is assigned on each incoming path). Vregs may
 * be used across blocks (also operands computed before a '&&'/'||').
 * Every use in a reachable block must be preceded by a definition on
 * every path from the entry (§8.4, ir_verify()); unreachable blocks are
 * not checked, so consumers must tolerate them. vreg_count is the
 * largest vreg + 1 and per-vreg tables are sized by it, so keep vreg
 * numbers dense (the text format caps them at 16777215).
 * Globals (@name, one per LIL variable, ids 0..global_count-1) start at 0
 * and are read and written only by 'load' and 'store'.
 *
 * Semantics (§8.2):
 *   const   d = imm                   copy d = a
 *   add/sub/mul  d = a OP b          wraps modulo 2^64 (§4.5)
 *   div     d = a / b                 truncates toward zero; b == 0, or
 *                                     a == INT64_MIN and b == -1, TRAPS
 *                                     (SIGFPE at run time, §4.6, §5.6)
 *   and/or  d = a & b, a | b          bitwise
 *   neg/not d = -a (wraps), ~a
 *   eq ne lt le gt ge  d = (a OP b) ? 1 : 0, signed comparison
 *   load    d = @g                    store @g = a
 *   putc    writes the byte a & 0xff to standard output (no result; the
 *           value of the LIL call putc(x) is x itself, §4.10)
 *   jmp L                             br c, L1, L2  (to L1 when c != 0)
 *   exit a  ends the program with exit status a & 0xff (§5.5)
 *
 * A div is never removable or foldable unless its operands are known not
 * to trap (§5.7). Instructions carry the source span they came from (a
 * LIL expression for lowered IR, the instruction's position for parsed
 * IR text); it is used only in diagnostics.
 */

typedef enum {
    IR_CONST,
    IR_COPY,
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_DIV,
    IR_AND,
    IR_OR,
    IR_NEG,
    IR_NOT,
    IR_EQ,
    IR_NE,
    IR_LT,
    IR_LE,
    IR_GT,
    IR_GE,
    IR_LOAD,
    IR_STORE,
    IR_PUTC,
    /* Terminators. */
    IR_JMP,
    IR_BR,
    IR_EXIT
} Ir_Opcode;

#define IR_OPCODE_COUNT ((int)IR_EXIT + 1)

/* "No vreg / block / global": never a valid index. */
#define IR_NONE SIZE_MAX

typedef struct {
    Ir_Opcode opcode;
    /* The defined vreg, or IR_NONE (store, putc, terminators). */
    size_t dest;
    /* Vreg operands: a, b (binary, compare), a (copy, neg, not, store,
     * putc, exit), the condition (br). Unused slots are IR_NONE. */
    size_t operands[2];
    /* IR_CONST: the value. */
    int64_t value;
    /* IR_LOAD, IR_STORE: the global id; IR_NONE otherwise. */
    size_t global;
    /* IR_JMP: targets[0]. IR_BR: targets[0] when the condition is
     * nonzero, targets[1] otherwise. Unused slots are IR_NONE. */
    size_t targets[2];
    Source_Span span;
} Ir_Instruction;

typedef struct {
    char *name;                 /* unique label, NUL-terminated */
    Source_Span span;
    Ir_Instruction *instructions;
    size_t count;
    size_t capacity;
} Ir_Block;

typedef struct {
    char *name;                 /* NUL-terminated */
} Ir_Global;

typedef struct {
    Ir_Global *globals;
    size_t global_count;
    size_t global_capacity;
    Ir_Block *blocks;           /* blocks[0] is the entry */
    size_t block_count;
    size_t block_capacity;
    /* Every vreg id in the program is < vreg_count. */
    size_t vreg_count;
} Ir_Program;

/* Operand shape of an opcode. */
typedef enum {
    IR_SHAPE_CONST,     /* %d = op IMM */
    IR_SHAPE_UNARY,     /* %d = op %a */
    IR_SHAPE_BINARY,    /* %d = op %a, %b */
    IR_SHAPE_LOAD,      /* %d = op @g */
    IR_SHAPE_STORE,     /* op @g, %a */
    IR_SHAPE_USE,       /* op %a (putc, exit) */
    IR_SHAPE_JMP,       /* op L */
    IR_SHAPE_BR         /* op %c, L1, L2 */
} Ir_Shape;

const char *ir_opcode_name(Ir_Opcode opcode);
/* The opcode spelled 'name' (length bytes); false when there is none. */
bool ir_opcode_from_name(const char *name, size_t length, Ir_Opcode *opcode);
Ir_Shape ir_opcode_shape(Ir_Opcode opcode);
bool ir_opcode_is_terminator(Ir_Opcode opcode);
/* Whether the opcode defines a vreg (dest). */
bool ir_opcode_has_dest(Ir_Opcode opcode);
/* Number of vreg operands (0..2). */
size_t ir_opcode_operand_count(Ir_Opcode opcode);
/* Number of block targets (0..2). */
size_t ir_opcode_target_count(Ir_Opcode opcode);

/* Instructions with every unused field set to IR_NONE. */
Ir_Instruction ir_make_const(size_t dest, int64_t value, Source_Span span);
Ir_Instruction ir_make_unary(Ir_Opcode opcode, size_t dest, size_t a,
                             Source_Span span);
Ir_Instruction ir_make_binary(Ir_Opcode opcode, size_t dest, size_t a,
                              size_t b, Source_Span span);
Ir_Instruction ir_make_load(size_t dest, size_t global, Source_Span span);
Ir_Instruction ir_make_store(size_t global, size_t a, Source_Span span);
Ir_Instruction ir_make_putc(size_t a, Source_Span span);
Ir_Instruction ir_make_jmp(size_t target, Source_Span span);
Ir_Instruction ir_make_br(size_t condition, size_t if_true, size_t if_false,
                          Source_Span span);
Ir_Instruction ir_make_exit(size_t a, Source_Span span);

void ir_program_init(Ir_Program *program);
/* Releases everything and re-initializes; safe to call repeatedly. */
void ir_program_free(Ir_Program *program);

/* Appends a global (copying name); its id is stored in *id. Returns false
 * when out of memory. */
bool ir_program_add_global(Ir_Program *program, const char *name,
                           size_t length, size_t *id);
/* Appends an empty block named 'name' (copied); its index is stored in
 * *index. Returns false when out of memory. Invalidates Ir_Block
 * pointers into the program. */
bool ir_program_add_block(Ir_Program *program, const char *name,
                          size_t length, Source_Span span, size_t *index);
/* A fresh vreg id (vreg_count++). */
size_t ir_program_new_vreg(Ir_Program *program);
/* Appends a copy of 'instruction'. Returns false when out of memory. */
bool ir_block_append(Ir_Block *block, const Ir_Instruction *instruction);
/* The block's terminator (its last instruction when that is one), or
 * NULL. */
const Ir_Instruction *ir_block_terminator(const Ir_Block *block);
/* The index of the block named 'name', or IR_NONE (linear search). */
size_t ir_program_find_block(const Ir_Program *program, const char *name,
                             size_t length);
/* The id of the global named 'name', or IR_NONE (linear search). */
size_t ir_program_find_global(const Ir_Program *program, const char *name,
                              size_t length);

/*
 * Writes the §8.3 text format. Stable: the output depends only on the
 * program, and ir_parse() of it rebuilds the same program, so
 * dump -> parse -> dump is the identity. The program should satisfy
 * ir_verify() (invalid references are printed as "?").
 */
void ir_program_dump(FILE *stream, const Ir_Program *program);

#endif
