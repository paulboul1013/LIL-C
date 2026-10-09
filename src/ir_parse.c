#include "ir_parse.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See include/ir_parse.h and docs/spec.md §8.3. */

/* Names longer than this are shortened in messages (like sema.c). */
#define PARSE_MAX_NAME_IN_MESSAGE 200

typedef enum {
    IR_TOKEN_END,
    IR_TOKEN_NEWLINE,
    IR_TOKEN_NAME,      /* label, opcode or 'global' */
    IR_TOKEN_GLOBAL,    /* @name; the name excludes the '@' */
    IR_TOKEN_VREG,      /* %N */
    IR_TOKEN_INTEGER,   /* -?digits */
    IR_TOKEN_COLON,
    IR_TOKEN_COMMA,
    IR_TOKEN_EQUALS,
    IR_TOKEN_ERROR
} Ir_Token_Kind;

typedef struct {
    Ir_Token_Kind kind;
    Source_Span span;
    const char *name;       /* NAME, GLOBAL: the name's bytes */
    size_t name_length;
    uint64_t number;        /* VREG */
    int64_t value;          /* INTEGER */
} Ir_Token;

/* Open-addressing map from names to indices (labels or globals). */
typedef struct {
    const char **names;
    size_t *lengths;
    size_t *values;
    size_t count;
    size_t capacity;        /* 0 or a power of two */
} Name_Map;

typedef struct {
    size_t block;
    size_t instruction;
    size_t slot;
    const char *name;
    size_t length;
} Fixup;

typedef struct {
    const Source *source;
    size_t offset;
    size_t line;
    size_t column;
    Ir_Token token;         /* the current token */
    bool has_peek;
    Ir_Token peek;
    Ir_Program *program;
    Diagnostic *error;
    bool failed;
    Name_Map blocks;
    Name_Map globals;
    Fixup *fixups;
    size_t fixup_count;
    size_t fixup_capacity;
} Ir_Parser;

/* ---- names ------------------------------------------------------------ */

static size_t hash_name(const char *name, size_t length) {
    size_t hash = 1469598103934665603u;

    for (size_t i = 0; i < length; ++i) {
        hash = (hash ^ (unsigned char)name[i]) * 1099511628211u;
    }

    return hash;
}

static size_t name_map_find(const Name_Map *map, const char *name,
                            size_t length) {
    if (map->capacity == 0) {
        return IR_NONE;
    }

    for (size_t i = hash_name(name, length) & (map->capacity - 1);;
         i = (i + 1) & (map->capacity - 1)) {
        if (map->names[i] == NULL) {
            return IR_NONE;
        }

        if (map->lengths[i] == length &&
            memcmp(map->names[i], name, length) == 0) {
            return map->values[i];
        }
    }
}

static bool name_map_insert(Name_Map *map, const char *name, size_t length,
                            size_t value);

static bool name_map_grow(Name_Map *map) {
    Name_Map grown;
    const size_t capacity = map->capacity == 0 ? 16 : map->capacity * 2;

    if (capacity > SIZE_MAX / sizeof(size_t)) {
        return false;
    }

    grown.names = calloc(capacity, sizeof(*grown.names));
    grown.lengths = calloc(capacity, sizeof(size_t));
    grown.values = calloc(capacity, sizeof(size_t));
    grown.count = 0;
    grown.capacity = capacity;

    bool ok = grown.names != NULL && grown.lengths != NULL &&
              grown.values != NULL;

    for (size_t i = 0; ok && i < map->capacity; ++i) {
        if (map->names[i] != NULL) {
            ok = name_map_insert(&grown, map->names[i], map->lengths[i],
                                 map->values[i]);
        }
    }

    if (!ok) {
        free(grown.names);
        free(grown.lengths);
        free(grown.values);
        return false;
    }

    free(map->names);
    free(map->lengths);
    free(map->values);
    *map = grown;
    return true;
}

/* Inserts a name that is not in the map yet. */
static bool name_map_insert(Name_Map *map, const char *name, size_t length,
                            size_t value) {
    if ((map->count + 1) * 2 > map->capacity && !name_map_grow(map)) {
        return false;
    }

    size_t i = hash_name(name, length) & (map->capacity - 1);

    while (map->names[i] != NULL) {
        i = (i + 1) & (map->capacity - 1);
    }

    map->names[i] = name;
    map->lengths[i] = length;
    map->values[i] = value;
    ++map->count;
    return true;
}

static void name_map_free(Name_Map *map) {
    free(map->names);
    free(map->lengths);
    free(map->values);
    memset(map, 0, sizeof(*map));
}

/* ---- errors ----------------------------------------------------------- */

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void parse_error(Ir_Parser *parser, Source_Span span,
                        const char *format, ...) {
    if (parser->failed) {
        return;
    }

    char message[sizeof(parser->error->message)];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    diagnostic_set(parser->error, DIAGNOSTIC_IR, span, "%s", message);
    parser->failed = true;
}

static void out_of_memory(Ir_Parser *parser, Source_Span span) {
    if (!parser->failed) {
        diagnostic_set(parser->error, DIAGNOSTIC_INTERNAL, span,
                       "out of memory while reading IR");
        parser->failed = true;
    }
}

static int shown_length(size_t length) {
    return length > PARSE_MAX_NAME_IN_MESSAGE ? PARSE_MAX_NAME_IN_MESSAGE
                                              : (int)length;
}

static const char *ellipsis(size_t length) {
    return length > PARSE_MAX_NAME_IN_MESSAGE ? "..." : "";
}

/* ---- lexer ------------------------------------------------------------ */

static bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '.';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_name_continue(char c) {
    return is_name_start(c) || is_digit(c);
}

static char current_char(const Ir_Parser *parser) {
    return parser->offset < parser->source->length
        ? parser->source->text[parser->offset]
        : '\0';
}

static bool at_end(const Ir_Parser *parser) {
    return parser->offset >= parser->source->length;
}

static void advance(Ir_Parser *parser) {
    if (parser->source->text[parser->offset] == '\n') {
        ++parser->line;
        parser->column = 1;
    } else {
        ++parser->column;
    }

    ++parser->offset;
}

static Source_Span span_from(const Ir_Parser *parser, size_t offset,
                             size_t line, size_t column) {
    return (Source_Span){offset, parser->offset - offset, line, column};
}

static void skip_digits(Ir_Parser *parser) {
    while (!at_end(parser) && is_digit(current_char(parser))) {
        advance(parser);
    }
}

static void skip_name(Ir_Parser *parser) {
    while (!at_end(parser) && is_name_continue(current_char(parser))) {
        advance(parser);
    }
}

/* Lexes '-'? digits as an int64 (§8.3: decimal, any leading zeros). */
static void lex_integer(Ir_Parser *parser, Ir_Token *token) {
    const size_t start = parser->offset;
    const bool negative = current_char(parser) == '-';

    if (negative) {
        advance(parser);

        if (at_end(parser) || !is_digit(current_char(parser))) {
            token->span = span_from(parser, start, token->span.line,
                                    token->span.column);
            parse_error(parser, token->span, "expected a digit after '-'");
            token->kind = IR_TOKEN_ERROR;
            return;
        }
    }

    const uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u
                                    : (uint64_t)INT64_MAX;
    uint64_t magnitude = 0;
    bool overflow = false;

    while (!at_end(parser) && is_digit(current_char(parser))) {
        const uint64_t digit = (uint64_t)(current_char(parser) - '0');

        if (magnitude > (limit - digit) / 10) {
            overflow = true;
        } else {
            magnitude = magnitude * 10 + digit;
        }

        advance(parser);
    }

    token->span = span_from(parser, start, token->span.line,
                            token->span.column);

    if (overflow) {
        parse_error(parser, token->span, "integer out of range");
        token->kind = IR_TOKEN_ERROR;
        return;
    }

    token->kind = IR_TOKEN_INTEGER;

    /* -magnitude without signed overflow: INT64_MIN is -(INT64_MAX) - 1. */
    if (!negative) {
        token->value = (int64_t)magnitude;
    } else if (magnitude == 0) {
        token->value = 0;
    } else {
        token->value = -(int64_t)(magnitude - 1u) - 1;
    }
}

static Ir_Token lex(Ir_Parser *parser) {
    Ir_Token token;

    memset(&token, 0, sizeof(token));

    for (;;) {
        const char c = current_char(parser);

        if (!at_end(parser) && (c == ' ' || c == '\t' || c == '\r')) {
            advance(parser);
        } else if (!at_end(parser) && c == ';') {
            while (!at_end(parser) && current_char(parser) != '\n') {
                advance(parser);
            }
        } else {
            break;
        }
    }

    const size_t start = parser->offset;
    const size_t line = parser->line;
    const size_t column = parser->column;
    token.span = (Source_Span){start, 0, line, column};

    if (at_end(parser)) {
        token.kind = IR_TOKEN_END;
        return token;
    }

    const char c = current_char(parser);

    if (c == '\n') {
        advance(parser);
        token.kind = IR_TOKEN_NEWLINE;
    } else if (is_name_start(c)) {
        skip_name(parser);
        token.kind = IR_TOKEN_NAME;
        token.name = parser->source->text + start;
        token.name_length = parser->offset - start;
    } else if (c == '@') {
        advance(parser);

        if (at_end(parser) || !is_name_start(current_char(parser))) {
            token.span = span_from(parser, start, line, column);
            parse_error(parser, token.span, "expected a name after '@'");
            token.kind = IR_TOKEN_ERROR;
            return token;
        }

        skip_name(parser);
        token.kind = IR_TOKEN_GLOBAL;
        token.name = parser->source->text + start + 1;
        token.name_length = parser->offset - start - 1;
    } else if (c == '%') {
        advance(parser);

        if (at_end(parser) || !is_digit(current_char(parser))) {
            token.span = span_from(parser, start, line, column);
            parse_error(parser, token.span, "expected a number after '%%'");
            token.kind = IR_TOKEN_ERROR;
            return token;
        }

        bool too_large = false;

        while (!at_end(parser) && is_digit(current_char(parser))) {
            token.number = token.number * 10 +
                           (uint64_t)(current_char(parser) - '0');
            too_large = too_large || token.number > IR_PARSE_MAX_VREG;

            if (too_large) {
                skip_digits(parser);
                break;
            }

            advance(parser);
        }

        token.span = span_from(parser, start, line, column);

        if (too_large) {
            parse_error(parser, token.span,
                        "virtual register number too large (the limit is "
                        "%u)", IR_PARSE_MAX_VREG);
            token.kind = IR_TOKEN_ERROR;
            return token;
        }

        token.kind = IR_TOKEN_VREG;
    } else if (c == '-' || is_digit(c)) {
        lex_integer(parser, &token);
        return token;
    } else if (c == ':' || c == ',' || c == '=') {
        advance(parser);
        token.kind = c == ':' ? IR_TOKEN_COLON
                   : c == ',' ? IR_TOKEN_COMMA
                              : IR_TOKEN_EQUALS;
    } else {
        const unsigned char byte = (unsigned char)c;

        advance(parser);
        token.span = span_from(parser, start, line, column);

        if (byte >= 0x20 && byte <= 0x7e) {
            parse_error(parser, token.span, "unexpected character '%c'", c);
        } else {
            parse_error(parser, token.span, "unexpected character '\\x%02x'",
                        byte);
        }

        token.kind = IR_TOKEN_ERROR;
        return token;
    }

    token.span = span_from(parser, start, line, column);
    return token;
}

static void next(Ir_Parser *parser) {
    if (parser->has_peek) {
        parser->token = parser->peek;
        parser->has_peek = false;
    } else {
        parser->token = lex(parser);
    }
}

static const Ir_Token *peek(Ir_Parser *parser) {
    if (!parser->has_peek) {
        parser->peek = lex(parser);
        parser->has_peek = true;
    }

    return &parser->peek;
}

/* ---- parser ----------------------------------------------------------- */

/* Consumes the current token if it has 'kind'; otherwise reports
 * 'message' at it. */
static bool expect(Ir_Parser *parser, Ir_Token_Kind kind,
                   const char *message) {
    if (parser->token.kind != kind) {
        parse_error(parser, parser->token.span, "%s", message);
        return false;
    }

    return true;
}

/* The current token as a vreg; keeps vreg_count above every vreg. */
static bool take_vreg(Ir_Parser *parser, size_t *vreg) {
    if (!expect(parser, IR_TOKEN_VREG, "expected a virtual register")) {
        return false;
    }

    *vreg = (size_t)parser->token.number;

    if (*vreg >= parser->program->vreg_count) {
        parser->program->vreg_count = *vreg + 1;
    }

    return true;
}

static bool parse_vreg(Ir_Parser *parser, size_t *vreg) {
    next(parser);
    return take_vreg(parser, vreg);
}

static bool parse_comma(Ir_Parser *parser) {
    next(parser);
    return expect(parser, IR_TOKEN_COMMA, "expected ','");
}

static bool parse_global_reference(Ir_Parser *parser, size_t *global) {
    next(parser);

    if (!expect(parser, IR_TOKEN_GLOBAL, "expected a global (@name)")) {
        return false;
    }

    /* Undeclared: IR_NONE, which the verifier reports (§8.4). */
    *global = name_map_find(&parser->globals, parser->token.name,
                            parser->token.name_length);
    return true;
}

/* A label reference; resolved once every label is known. */
static bool parse_target(Ir_Parser *parser, size_t block, size_t slot) {
    next(parser);

    if (!expect(parser, IR_TOKEN_NAME, "expected a block name")) {
        return false;
    }

    if (parser->fixup_count == parser->fixup_capacity) {
        const size_t capacity =
            parser->fixup_capacity == 0 ? 16 : parser->fixup_capacity * 2;
        Fixup *grown = capacity > SIZE_MAX / sizeof(Fixup)
            ? NULL
            : realloc(parser->fixups, capacity * sizeof(Fixup));

        if (grown == NULL) {
            out_of_memory(parser, parser->token.span);
            return false;
        }

        parser->fixups = grown;
        parser->fixup_capacity = capacity;
    }

    parser->fixups[parser->fixup_count++] = (Fixup){
        block,
        parser->program->blocks[block].count,
        slot,
        parser->token.name,
        parser->token.name_length,
    };
    return true;
}

/* The operands of 'instruction' (its opcode is set) for block 'block'. */
static bool parse_operands(Ir_Parser *parser, Ir_Instruction *instruction,
                           size_t block) {
    switch (ir_opcode_shape(instruction->opcode)) {
        case IR_SHAPE_CONST:
            next(parser);

            if (!expect(parser, IR_TOKEN_INTEGER, "expected an integer")) {
                return false;
            }

            instruction->value = parser->token.value;
            return true;

        case IR_SHAPE_UNARY:
        case IR_SHAPE_USE:
            return parse_vreg(parser, &instruction->operands[0]);

        case IR_SHAPE_BINARY:
            return parse_vreg(parser, &instruction->operands[0]) &&
                   parse_comma(parser) &&
                   parse_vreg(parser, &instruction->operands[1]);

        case IR_SHAPE_LOAD:
            return parse_global_reference(parser, &instruction->global);

        case IR_SHAPE_STORE:
            return parse_global_reference(parser, &instruction->global) &&
                   parse_comma(parser) &&
                   parse_vreg(parser, &instruction->operands[0]);

        case IR_SHAPE_JMP:
            return parse_target(parser, block, 0);

        case IR_SHAPE_BR:
            break;
    }

    return parse_vreg(parser, &instruction->operands[0]) &&
           parse_comma(parser) && parse_target(parser, block, 0) &&
           parse_comma(parser) && parse_target(parser, block, 1);
}

/*
 * One instruction starting at the current token: '%N = op ...' or
 * 'op ...'. 'block' is the current block, or IR_NONE before the first
 * label.
 */
static void parse_instruction(Ir_Parser *parser, size_t block) {
    const Source_Span start = parser->token.span;
    size_t dest = IR_NONE;

    if (block == IR_NONE) {
        parse_error(parser, start, "instruction outside of a block");
        return;
    }

    if (parser->token.kind == IR_TOKEN_VREG) {
        if (!take_vreg(parser, &dest)) {
            return;
        }

        next(parser);

        if (!expect(parser, IR_TOKEN_EQUALS, "expected '='")) {
            return;
        }

        next(parser);

        if (!expect(parser, IR_TOKEN_NAME, "expected an instruction name")) {
            return;
        }
    }

    Ir_Opcode opcode;
    const Ir_Token name = parser->token;

    if (!ir_opcode_from_name(name.name, name.name_length, &opcode)) {
        parse_error(parser, name.span, "unknown instruction '%.*s%s'",
                    shown_length(name.name_length), name.name,
                    ellipsis(name.name_length));
        return;
    }

    if (ir_opcode_has_dest(opcode) && dest == IR_NONE) {
        parse_error(parser, name.span,
                    "'%s' needs a destination register ('%%N = %s ...')",
                    ir_opcode_name(opcode), ir_opcode_name(opcode));
        return;
    }

    if (!ir_opcode_has_dest(opcode) && dest != IR_NONE) {
        parse_error(parser, name.span, "'%s' does not produce a value",
                    ir_opcode_name(opcode));
        return;
    }

    Ir_Instruction instruction = ir_make_jmp(IR_NONE, start);
    instruction.opcode = opcode;
    instruction.dest = dest;

    if (!parse_operands(parser, &instruction, block)) {
        return;
    }

    if (!ir_block_append(&parser->program->blocks[block], &instruction)) {
        out_of_memory(parser, start);
    }
}

/* 'global @name' (only before the first block). */
static void parse_global_declaration(Ir_Parser *parser, bool after_block) {
    const Source_Span start = parser->token.span;

    if (after_block) {
        parse_error(parser, start, "global declaration after the first block");
        return;
    }

    next(parser);

    if (!expect(parser, IR_TOKEN_GLOBAL, "expected a global (@name)")) {
        return;
    }

    const Ir_Token name = parser->token;

    if (name_map_find(&parser->globals, name.name, name.name_length) !=
        IR_NONE) {
        parse_error(parser, name.span, "duplicate global '@%.*s%s'",
                    shown_length(name.name_length), name.name,
                    ellipsis(name.name_length));
        return;
    }

    size_t id = 0;
    Ir_Program *program = parser->program;

    if (!ir_program_add_global(program, name.name, name.name_length, &id) ||
        !name_map_insert(&parser->globals, program->globals[id].name,
                         name.name_length, id)) {
        out_of_memory(parser, name.span);
    }
}

/* 'name:' starts a new block; returns its index (IR_NONE on error). */
static size_t parse_label(Ir_Parser *parser) {
    const Ir_Token name = parser->token;

    next(parser);   /* the ':' */

    if (name_map_find(&parser->blocks, name.name, name.name_length) !=
        IR_NONE) {
        parse_error(parser, name.span, "duplicate block '%.*s%s'",
                    shown_length(name.name_length), name.name,
                    ellipsis(name.name_length));
        return IR_NONE;
    }

    size_t index = 0;
    Ir_Program *program = parser->program;

    if (!ir_program_add_block(program, name.name, name.name_length,
                              name.span, &index) ||
        !name_map_insert(&parser->blocks, program->blocks[index].name,
                         name.name_length, index)) {
        out_of_memory(parser, name.span);
        return IR_NONE;
    }

    return index;
}

static bool is_keyword(const Ir_Token *token, const char *keyword) {
    return token->kind == IR_TOKEN_NAME &&
           token->name_length == strlen(keyword) &&
           memcmp(token->name, keyword, token->name_length) == 0;
}

static void parse_line(Ir_Parser *parser, size_t *block) {
    if (parser->token.kind == IR_TOKEN_NAME &&
        peek(parser)->kind == IR_TOKEN_COLON) {
        const size_t index = parse_label(parser);

        if (index != IR_NONE) {
            *block = index;
        }
    } else if (is_keyword(&parser->token, "global")) {
        parse_global_declaration(parser, *block != IR_NONE);
    } else if (parser->token.kind == IR_TOKEN_NAME ||
               parser->token.kind == IR_TOKEN_VREG) {
        parse_instruction(parser, *block);
    } else {
        parse_error(parser, parser->token.span,
                    "expected an instruction, a label or a global "
                    "declaration");
    }

    if (parser->failed) {
        return;
    }

    next(parser);

    if (parser->token.kind != IR_TOKEN_NEWLINE &&
        parser->token.kind != IR_TOKEN_END) {
        parse_error(parser, parser->token.span, "expected end of line");
    }
}

static void resolve_fixups(Ir_Parser *parser) {
    for (size_t i = 0; i < parser->fixup_count; ++i) {
        const Fixup *fixup = &parser->fixups[i];

        /* Undefined labels stay IR_NONE for the verifier (§8.4). */
        parser->program->blocks[fixup->block]
            .instructions[fixup->instruction]
            .targets[fixup->slot] =
            name_map_find(&parser->blocks, fixup->name, fixup->length);
    }
}

bool ir_parse(const Source *source, Ir_Program *out, Diagnostic *error) {
    Ir_Parser parser;

    memset(&parser, 0, sizeof(parser));
    parser.source = source;
    parser.line = 1;
    parser.column = 1;
    parser.program = out;
    parser.error = error;
    diagnostic_clear(error);
    ir_program_init(out);

    size_t block = IR_NONE;

    for (next(&parser); !parser.failed && parser.token.kind != IR_TOKEN_END;
         next(&parser)) {
        if (parser.token.kind != IR_TOKEN_NEWLINE) {
            parse_line(&parser, &block);

            if (parser.token.kind == IR_TOKEN_END) {
                break;
            }
        }
    }

    if (!parser.failed) {
        resolve_fixups(&parser);
    }

    name_map_free(&parser.blocks);
    name_map_free(&parser.globals);
    free(parser.fixups);

    if (parser.failed) {
        ir_program_free(out);
        return false;
    }

    return true;
}
