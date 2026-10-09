#include "token.h"

const char* token_kind_name(Token_Kind kind) {
    switch (kind) {
    case TOKEN_END:
        return "End";

    case TOKEN_INVALID:
        return "Invalid";

    case TOKEN_IDENTIFIER:
        return "Identifier";

    case TOKEN_NUMBER:
        return "Number";

    case TOKEN_CHARACTER:
        return "Character";

    case TOKEN_IF:
        return "If";

    case TOKEN_ELSE:
        return "Else";

    case TOKEN_WHILE:
        return "While";

    case TOKEN_DO:
        return "Do";

    case TOKEN_BREAK:
        return "Break";

    case TOKEN_CONTINUE:
        return "Continue";

    case TOKEN_LEFT_PAREN:
        return "LeftParen";

    case TOKEN_RIGHT_PAREN:
        return "RightParen";

    case TOKEN_LEFT_BRACE:
        return "LeftBrace";

    case TOKEN_RIGHT_BRACE:
        return "RightBrace";

    case TOKEN_SEMICOLON:
        return "Semicolon";

    case TOKEN_COMMA:
        return "Comma";

    case TOKEN_ASSIGN:
        return "Assign";

    case TOKEN_PLUS:
        return "Plus";

    case TOKEN_MINUS:
        return "Minus";

    case TOKEN_STAR:
        return "Star";

    case TOKEN_SLASH:
        return "Slash";

    case TOKEN_AMPERSAND:
        return "Ampersand";

    case TOKEN_PIPE:
        return "Pipe";

    case TOKEN_TILDE:
        return "Tilde";

    case TOKEN_LOGICAL_AND:
        return "LogicalAnd";

    case TOKEN_LOGICAL_OR:
        return "LogicalOr";

    case TOKEN_LESS:
        return "Less";

    case TOKEN_LESS_EQUAL:
        return "LessEqual";

    case TOKEN_GREATER:
        return "Greater";

    case TOKEN_GREATER_EQUAL:
        return "GreaterEqual";

    case TOKEN_EQUAL_EQUAL:
        return "EqualEqual";

    case TOKEN_NOT_EQUAL:
        return "NotEqual";
    }

    return "Unknown";
}
