#ifndef LIL_IR_PARSE_H
#define LIL_IR_PARSE_H

#include "diagnostic.h"
#include "ir.h"
#include "source.h"

#include <stdbool.h>

/* Largest vreg number the text format accepts (§8.3), so a short file
 * cannot ask for a huge register file. */
#define IR_PARSE_MAX_VREG 16777215u

/*
 * Reads the §8.3 IR text format -- exactly what ir_program_dump() writes,
 * plus ';' comments, blank lines and free spacing -- into 'out' (which it
 * initializes). Stops at the first error and reports it in *error (stage
 * DIAGNOSTIC_IR, with the line and column in 'source'); 'out' is then
 * empty.
 *
 * Name resolution is left to the verifier: a jump to an undefined label
 * or a load/store of an undeclared global parses, with the target or
 * global set to IR_NONE, and ir_verify() rejects it (§8.4 rule 2).
 * Duplicate labels and duplicate globals are parse errors.
 */
bool ir_parse(const Source *source, Ir_Program *out, Diagnostic *error);

#endif
