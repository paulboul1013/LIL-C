#include "parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void parser_set_error(Parser *parser,const char *format,...) {
    if (parser->had_error) {
        return;
    }

    parser->had_error = true;

    va_list args;
    va_start(args,format);

    vsnprintf(parser->error_message,sizeof(parser->error_message),format,args);

    va_end(args);
}


static bool parser_advance(Parser *parser) {
    const Token token=lexer_next_token(parser->lexer);
    
    if (token.kind==TOKEN_INVALID) {
        parser_set_error(
            parser,
            "%s",
            lexer_error_message(parser->lexer)
        );

        return false;
    }

    parser->current = token;
    
    return true;
}

bool parser_init(Parser *parser,Lexer *lexer) {
    parser->lexer = lexer;

    parser->had_error = false;
    parser->error_message[0]='\0';

    memset(&parser->current,0,sizeof(parser->current));

    return parser_advance(parser);
}


static bool token_is_operand(Token_Kind kind) {
    switch (kind) {
        case TOKEN_IDENTIFIER:
        case TOKEN_NUMBER:
        case TOKEN_CHARACTER:
            return true;
        default:
            return false;
    }
}

static bool token_is_flat_operator(Token_Kind kind) {
    switch (kind) {
        case TOKEN_ASSIGN:
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_STAR:
        case TOKEN_SLASH:
            return true;
        default:
            return false;
    }
}

static bool flat_expression_push_operation(Flat_Expression *expression, Flat_Operation operation ) {
    if (expression->operation_count==expression->operation_capacity) {
        size_t new_capacity;

        if (expression->operation_capacity==0) {
            new_capacity=4;
        } else {
            new_capacity = expression->operation_capacity * 2;
        }

        Flat_Operation *new_operations = realloc(expression->operations,new_capacity*sizeof(Flat_Operation));
    
        if (new_operations==NULL) {
            return false;
        }

        expression->operations = new_operations;

        expression->operation_capacity=new_capacity;
    
    }

    expression->operations[expression->operation_count] = operation;

    ++expression->operation_count;

    return true;
}

static void flat_expression_init(Flat_Expression *expression) {
    memset(expression,0,sizeof(*expression));
}

bool parser_parse_flat_expression(Parser *parser,Flat_Expression *expression) {
    flat_expression_init(expression);

    //expression first token must be an operand
    if (!token_is_operand(parser->current.kind)) {
        parser_set_error(
            parser,
            "%zu:%zu: expected operand",
            parser->current.line,
            parser->current.column
        );


        return false;
    }

    //save first operand 
    expression->first = parser->current;

    if (!parser_advance(parser)) {
        return false;
    }

    //repeat read operator and operand
    while (token_is_flat_operator(parser->current.kind)) {
        Flat_Operation operation={0};

        operation.operator_token = parser->current;

        if (!parser_advance(parser)) {
            return false;
        }

        
        //after operator must have operand

        if (!token_is_operand(parser->current.kind)) {
            parser_set_error(
                parser,
                "%zu:%zu: expected operand after operator",
                parser->current.line,
                parser->current.column
            );

            return false;
        }

        operation.operand = parser->current;

        if (!flat_expression_push_operation(expression,operation)) {
            parser_set_error(
                parser,
                "out of memory while parsing expression"
            );

            return false;
        }

        //advance to next token
        if (!parser_advance(parser)) {
            return false;
        }
        
    }


    
    //expression must have a ';' for ending
    if (parser->current.kind!=TOKEN_SEMICOLON) {
        parser_set_error(
            parser,
            "%zu:%zu: expected ';' after expression",
            parser->current.line,
            parser->current.column
        );

        return false;
    }

        
    //advance to ;
    if (!parser_advance(parser)) {
        return false;
    }


    return true;
}


void flat_expression_free(Flat_Expression *expression){
    free(expression->operations);

    expression->operations = NULL;
    expression->operation_count = 0;
    expression->operation_capacity = 0;
}

const char *parser_error_message(const Parser *parser) {
    return parser->error_message;
}

static void print_token_ascii(const Token *token) {
    putchar('"');

    fwrite(token->start,1,token->length,stdout);

    putchar('"');
}

void flat_expression_print_ascii(const Flat_Expression *expression){
    printf("Flat_Expression\n");
    printf("|\n");

    printf("+-- first\n");
    printf("|   `-- Token(");

    print_token_ascii(&expression->first);

    printf(")\n");

    printf("|\n");
    printf("+-- operations\n");

    if (expression->operation_count==0) {
        printf("|   `-- (empty)\n");
    }

    for(size_t i=0;i<expression->operation_count;i++){
        const Flat_Operation *operation = &expression->operations[i];

        const bool is_last = i + 1 == expression->operation_count;

        if (is_last) {
            printf("|   `-- [%zu]\n",i);

            printf("|       +-- operator ");

            print_token_ascii(&operation->operator_token);

            putchar('\n');

            printf("|       `-- operand  ");

            print_token_ascii(&operation->operand);

            putchar('\n');
        } else {
            printf("|   +-- [%zu]\n",i);

            printf("|   |   +-- operator ");

            print_token_ascii(&operation->operator_token);

            putchar('\n');

            printf("|   |   `-- operand  ");

            print_token_ascii(&operation->operand);

            putchar('\n');
        }
    }

    printf("|\n");

    printf("+-- operation_count = %zu\n",expression->operation_count);

    printf("`-- operation_capacity = %zu\n",expression->operation_capacity);
}