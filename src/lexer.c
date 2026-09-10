#include "lexer.h"

#include <ctype.h>
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

static bool is_identifier_start(char c) {
    const unsigned char value = (unsigned char )c;
    
    return isalpha(value) || c=='_' || c=='.';
}

static bool is_identifer_continue(char c) {
    const unsigned char value = (unsigned char)c;

    return isalnum(value) || c=='_' || c=='.';
}


static bool ignore_upper_and_lower_case(const char *start,size_t length,const char *expected) {
    const size_t expected_length = strlen(expected);

    if (length != expected_length) {
        return false;
    }

    for (size_t i=0;i<length;i++) {
        const unsigned char left = (unsigned char) start[i];
        const unsigned char right= (unsigned char) expected[i];

        if (tolower(left)!=tolower(right)) {
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

static Token lexer_scan_number(Lexer *lexer,size_t start,size_t line,size_t column) {
    while (isdigit((unsigned char)lexer_peek(lexer))) {
        lexer_advance(lexer);
    }

    return lexer_make_token(lexer,TOKEN_NUMBER,start,line,column);
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
        lexer_advance(lexer);
    }

    if (!lexer_match(lexer,'\'')) {
        return lexer_error_token(
            lexer,
            start,
            line,
            column,
            "phase-one LIL supports only one-character literals"
        );
    }

    return lexer_make_token(
        lexer,
        TOKEN_CHARACTER,
        start,
        line,
        column
    );
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

    if (isdigit((unsigned char)current)) {
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

            snprintf(
                message,
                sizeof(message),
                "unexpected character '%c'",
                current
            );

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
