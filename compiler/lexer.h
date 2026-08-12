#ifndef LEXER_H
#define LEXER_H

typedef enum {
    /* literals */
    TOK_NUMBER,
    TOK_STRING,
    TOK_IDENT,

    /* sigils */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_LANGLE,     /* < */
    TOK_RANGLE,     /* > */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_PERCENT,    /* % */

    /* operators */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_EQUAL,

    /* keywords — single words */
    TOK_SPRITE,
    TOK_STAGE,
    TOK_WHEN,
    TOK_VAR,
    TOK_LIST,
    TOK_FOREVER,
    TOK_REPEAT,
    TOK_IF,
    TOK_ELSE,
    TOK_NOT,
    TOK_AND,
    TOK_OR,
    TOK_MOD,
    TOK_TO,
    TOK_OF,
    TOK_BY,
    TOK_FOR,
    TOK_AT,
    TOK_WITH,
    TOK_STEPS,
    TOK_DEGREES,
    TOK_SECONDS,
    TOK_SECS,
    TOK_CONTAINS,

    /* flags */
    TOK_FLAG_GLOBAL,   /* --global */

    TOK_EOF,
    TOK_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char     *value;   /* heap-allocated, caller frees via token_free */
    int       line;
} Token;

typedef struct {
    const char *src;
    int         pos;
    int         line;
} Lexer;

void  lexer_init(Lexer *l, const char *src);
Token lexer_next(Lexer *l);
Token lexer_peek(Lexer *l);
void  token_free(Token *t);
const char *token_type_name(TokenType t);

#endif
