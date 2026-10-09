#include "lexer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    Token_Kind kind;
} Keyword;

static const Keyword keywords[] = {
    {"if",TOKEN_IF},
    {"else",     TOKEN_ELSE},
    {"while",    TOKEN_WHILE},
    {"do",       TOKEN_DO},
    {"break",    TOKEN_BREAK},
    {"continue", TOKEN_CONTINUE}
};

void lexer_init(Lexer *lexer, const Source *source) {
    lexer->source = source;

    lexer->position = 0;
    lexer->line = 1;
    lexer->column = 1;

    diagnostic_clear(&lexer->diagnostic);
}

const Diagnostic *lexer_diagnostic(const Lexer *lexer) {
    return &lexer->diagnostic;
}

static bool lexer_is_at_end(const Lexer *lexer) {
    return lexer->position >= lexer->source->length;
}

static char lexer_peek(const Lexer *lexer) {
    if (lexer_is_at_end(lexer)) {
        return '\0';
    }

    return lexer->source->text[lexer->position];
}

static char lexer_advance(Lexer *lexer) {
    if (lexer_is_at_end(lexer)) {
        return '\0';
    }

    const char current = lexer->source->text[lexer->position];

    ++lexer->position;

    if (current=='\n'){
        ++lexer->line;
        lexer->column = 1;
    } else {
        ++lexer->column;
    }

    return current;
}

static bool lexer_match(Lexer *lexer,char expected) {
    if (lexer_is_at_end(lexer)) {
        return false;
    }

    if (lexer_peek(lexer)!=expected) {
        return false;
    }

    lexer_advance(lexer);
    return true;
}

static Token lexer_make_token(const Lexer *lexer,Token_Kind kind,size_t start ,size_t line, size_t column) {
    Token token;
    
    token.kind = kind;
    token.span.offset = start;
    token.span.length = lexer->position - start;
    token.span.line = line;
    token.span.column = column;
    token.value = 0;

    return token;
}

static Token lexer_error_token(Lexer *lexer, size_t start, size_t line ,size_t column, const char *message) {
    const Token token = lexer_make_token(
        lexer,
        TOKEN_INVALID,
        start,
        line,
        column
    );

    diagnostic_set(
        &lexer->diagnostic,
        DIAGNOSTIC_LEXICAL,
        token.span,
        "%s",
        message
    );

    return token;
}

static void lexer_skip_ignored(Lexer *lexer) {
    while (!lexer_is_at_end(lexer)) {
        const char current = lexer_peek(lexer);

        switch (current) {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                lexer_advance(lexer);
                break;

            case '%': //single comment
                while(!lexer_is_at_end(lexer) && lexer_peek(lexer)!='\n') {
                    lexer_advance(lexer);
                }
                break;

            default:
                return;
        }
    }
}

/*
 * §2.4: only ASCII letters and digits count. These are explicit range
 * checks instead of <ctype.h> so the result never depends on the locale.
 */
static bool is_ascii_letter(char c) {
    return (c>='a' && c<='z') || (c>='A' && c<='Z');
}

static bool is_ascii_digit(char c) {
    return c>='0' && c<='9';
}

static char ascii_to_lower(char c) {
    return (c>='A' && c<='Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_identifier_start(char c) {
    return is_ascii_letter(c) || c=='_' || c=='.';
}

static bool is_identifer_continue(char c) {
    return is_ascii_letter(c) || is_ascii_digit(c) || c=='_' || c=='.';
}


static bool ignore_upper_and_lower_case(const char *start,size_t length,const char *expected) {
    const size_t expected_length = strlen(expected);

    if (length != expected_length) {
        return false;
    }

    for (size_t i=0;i<length;i++) {
        if (ascii_to_lower(start[i])!=ascii_to_lower(expected[i])) {
            return false;
        }
    }

    return true;
}


static Token_Kind Identifier_kind(const char *start,size_t length) {
    const size_t keywords_count = sizeof(keywords)/sizeof(keywords[0]);

    for(size_t i=0;i<keywords_count;i++){
        if (ignore_upper_and_lower_case(start,length,keywords[i].name)) {
            return keywords[i].kind;
        }
    }

    return TOKEN_IDENTIFIER;
}

static Token lexer_scan_identifier(Lexer *lexer,size_t start,size_t line,size_t column) {
    while(is_identifer_continue(lexer_peek(lexer))) {
        lexer_advance(lexer);
    }

    const size_t length = lexer->position - start;
    
    const Token_Kind kind = Identifier_kind(
        lexer->source->text + start,
        length
    );

    return lexer_make_token(lexer,kind,start,line,column);
}

/*
 * §2.7: the longest digit string is one literal. A leading '0' followed by
 * more digits makes it octal (§2.7.2); an '8' or '9' in it is an error,
 * checked before the INT64_MAX range (§2.7.3). Both errors cover the whole
 * digit string.
 */
static Token lexer_scan_number(Lexer *lexer,size_t start,size_t line,size_t column) {
    while (is_ascii_digit(lexer_peek(lexer))) {
        lexer_advance(lexer);
    }

    const char *digits = lexer->source->text + start;
    const size_t length = lexer->position - start;
    const bool is_octal = length > 1 && digits[0] == '0';
    const int64_t base = is_octal ? 8 : 10;

    if (is_octal) {
        for (size_t i = 1; i < length; ++i) {
            if (digits[i] == '8' || digits[i] == '9') {
                char message[64];

                snprintf(
                    message,
                    sizeof(message),
                    "invalid digit '%c' in octal literal",
                    digits[i]
                );

                return lexer_error_token(lexer, start, line, column, message);
            }
        }
    }

    int64_t value = 0;

    for (size_t i = 0; i < length; ++i) {
        const int64_t digit = digits[i] - '0';

        if (value > (INT64_MAX - digit) / base) {
            return lexer_error_token(
                lexer,
                start,
                line,
                column,
                "integer literal out of range"
            );
        }

        value = value * base + digit;
    }

    Token token = lexer_make_token(lexer,TOKEN_NUMBER,start,line,column);
    token.value = value;
    return token;
}

/* §2.6 the value of an escape sequence's second character. */
static int64_t escape_value(char c) {
    switch (c) {
        case '0':
            return 0;
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        default:
            return (unsigned char)c;   /* '\\' and '\'' stand for themselves */
    }
}

static bool is_supported_escape(char c) {
    switch (c) {
        case '0':
        case 'n':
        case 'r':
        case 't':
        case '\\':
        case '\'':
            return true;
        default:
            return false;
    }
}

static Token lexer_scan_character(Lexer *lexer,size_t start,size_t line,size_t column) {
    int64_t value = 0;

    if (lexer_is_at_end(lexer) || lexer_peek(lexer)=='\n') {
        return lexer_error_token(
            lexer,
            start,
            line,
            column,
            "unterminated character literal"
        );
    }

    if (lexer_peek(lexer)=='\'') {
        lexer_advance(lexer);

        return lexer_error_token(
            lexer,
            start,
            line,
            column,
            "empty character literal"
        );
    }

    if (lexer_peek(lexer)=='\\') {
        lexer_advance(lexer);

        if (lexer_is_at_end(lexer) || lexer_peek(lexer)=='\n') {
            return lexer_error_token(
                lexer,
                start,
                line,
                column,
                "unterminated character literal"
            );
        }

        const char escaped = lexer_advance(lexer);

        value = escape_value(escaped);

        if (!is_supported_escape(escaped)) {
            return lexer_error_token(
                lexer,
                start,
                line,
                column,
                "unsupported escape sequence"
            );
        }
    } else {
        value = (unsigned char)lexer_advance(lexer);
    }

    if (lexer_is_at_end(lexer) || lexer_peek(lexer)=='\n') {
        return lexer_error_token(
            lexer,
            start,
            line,
            column,
            "unterminated character literal"
        );
    }

    if (!lexer_match(lexer,'\'')) {
        /* §2.6: the span runs through the second character. */
        lexer_advance(lexer);

        return lexer_error_token(
            lexer,
            start,
            line,
            column,
            "character literal contains more than one character"
        );
    }

    Token token = lexer_make_token(
        lexer,
        TOKEN_CHARACTER,
        start,
        line,
        column
    );
    token.value = value;
    return token;
}

Token lexer_next_token(Lexer *lexer) {
    lexer_skip_ignored(lexer);

    const size_t start = lexer->position;
    const size_t start_line = lexer->line;
    const size_t start_column = lexer->column;

    if (lexer_is_at_end(lexer)) {
        return lexer_make_token(
            lexer,
            TOKEN_END,
            start,
            start_line,
            start_column
        );
    }

    const char current = lexer_advance(lexer);

    if (is_identifier_start(current)) {
        return lexer_scan_identifier(
            lexer,
            start,
            start_line,
            start_column
        );
    }

    if (is_ascii_digit(current)) {
        return lexer_scan_number(
            lexer,
            start,
            start_line,
            start_column
        );
    }

    switch (current) {
        case '\'':
            return lexer_scan_character(
                lexer,
                start,
                start_line,
                start_column
            );

        case '(':
            return lexer_make_token(
                lexer,
                TOKEN_LEFT_PAREN,
                start,
                start_line,
                start_column
            );

        case ')':
            return lexer_make_token(
                lexer,
                TOKEN_RIGHT_PAREN,
                start,
                start_line,
                start_column
            );

        case '{':
            return lexer_make_token(
                lexer,
                TOKEN_LEFT_BRACE,
                start,
                start_line,
                start_column
            );

        case '}':
            return lexer_make_token(
                lexer,
                TOKEN_RIGHT_BRACE,
                start,
                start_line,
                start_column
            );

        case ';':
            return lexer_make_token(
                lexer,
                TOKEN_SEMICOLON,
                start,
                start_line,
                start_column
            );

        case ',':
            return lexer_make_token(
                lexer,
                TOKEN_COMMA,
                start,
                start_line,
                start_column
            );

        case '+':
            return lexer_make_token(
                lexer,
                TOKEN_PLUS,
                start,
                start_line,
                start_column
            );

        case '-':
            return lexer_make_token(
                lexer,
                TOKEN_MINUS,
                start,
                start_line,
                start_column
            );

        case '*':
            return lexer_make_token(
                lexer,
                TOKEN_STAR,
                start,
                start_line,
                start_column
            );

        case '/':
            return lexer_make_token(
                lexer,
                TOKEN_SLASH,
                start,
                start_line,
                start_column
            );

        case '=':
            return lexer_make_token(
                lexer,
                lexer_match(lexer, '=')
                    ? TOKEN_EQUAL_EQUAL
                    : TOKEN_ASSIGN,
                start,
                start_line,
                start_column
            );

        case '~':
            return lexer_make_token(
                lexer,
                lexer_match(lexer, '=')
                    ? TOKEN_NOT_EQUAL
                    : TOKEN_TILDE,
                start,
                start_line,
                start_column
            );

        case '<':
            return lexer_make_token(
                lexer,
                lexer_match(lexer, '=')
                    ? TOKEN_LESS_EQUAL
                    : TOKEN_LESS,
                start,
                start_line,
                start_column
            );

        case '>':
            return lexer_make_token(
                lexer,
                lexer_match(lexer, '=')
                    ? TOKEN_GREATER_EQUAL
                    : TOKEN_GREATER,
                start,
                start_line,
                start_column
            );

        case '&':
            return lexer_make_token(
                lexer,
                lexer_match(lexer, '&')
                    ? TOKEN_LOGICAL_AND
                    : TOKEN_AMPERSAND,
                start,
                start_line,
                start_column
            );

        case '|':
            return lexer_make_token(
                lexer,
                lexer_match(lexer, '|')
                    ? TOKEN_LOGICAL_OR
                    : TOKEN_PIPE,
                start,
                start_line,
                start_column
            );

        default: {
            char message[64];
            const unsigned char byte = (unsigned char)current;

            /* §2.8: bytes outside printable ASCII are shown as '\xHH'. */
            if (byte < 0x20 || byte >= 0x7f) {
                snprintf(
                    message,
                    sizeof(message),
                    "unexpected character '\\x%02x'",
                    (unsigned)byte
                );
            } else {
                snprintf(
                    message,
                    sizeof(message),
                    "unexpected character '%c'",
                    current
                );
            }

            return lexer_error_token(
                lexer,
                start,
                start_line,
                start_column,
                message
            );
        }

    }

}
