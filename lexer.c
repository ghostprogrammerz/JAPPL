#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "lexer.h"

void lexer_init(Lexer *l, const char *src) {
    l->src  = src;
    l->pos  = 0;
    l->line = 1;
}

static void skip_whitespace_and_comments(Lexer *l) {
    while (1) {
        /* whitespace */
        while (l->src[l->pos] && isspace((unsigned char)l->src[l->pos])) {
            if (l->src[l->pos] == '\n') l->line++;
            l->pos++;
        }
        /* line comment */
        if (l->src[l->pos] == '/' && l->src[l->pos+1] == '/') {
            while (l->src[l->pos] && l->src[l->pos] != '\n') l->pos++;
            continue;
        }
        /* block comment */
        if (l->src[l->pos] == '/' && l->src[l->pos+1] == '*') {
            l->pos += 2;
            while (l->src[l->pos] && !(l->src[l->pos] == '*' && l->src[l->pos+1] == '/')) {
                if (l->src[l->pos] == '\n') l->line++;
                l->pos++;
            }
            if (l->src[l->pos]) l->pos += 2;
            continue;
        }
        break;
    }
}

static Token make_tok(TokenType type, const char *val, int line) {
    Token t;
    t.type  = type;
    t.value = val ? strdup(val) : NULL;
    t.line  = line;
    return t;
}

static struct { const char *word; TokenType type; } KEYWORDS[] = {
    {"sprite",   TOK_SPRITE},
    {"stage",    TOK_STAGE},
    {"when",     TOK_WHEN},
    {"var",      TOK_VAR},
    {"list",     TOK_LIST},
    {"forever",  TOK_FOREVER},
    {"repeat",   TOK_REPEAT},
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"not",      TOK_NOT},
    {"and",      TOK_AND},
    {"or",       TOK_OR},
    {"mod",      TOK_MOD},
    {"to",       TOK_TO},
    {"of",       TOK_OF},
    {"by",       TOK_BY},
    {"for",      TOK_FOR},
    {"at",       TOK_AT},
    {"with",     TOK_WITH},
    {"steps",    TOK_STEPS},
    {"degrees",  TOK_DEGREES},
    {"seconds",  TOK_SECONDS},
    {"secs",     TOK_SECS},
    {"contains", TOK_CONTAINS},
    {NULL, TOK_UNKNOWN}
};

Token lexer_next(Lexer *l) {
    skip_whitespace_and_comments(l);
    int line = l->line;
    char c = l->src[l->pos];

    if (!c) return make_tok(TOK_EOF, NULL, line);

    /* --global flag */
    if (c == '-' && l->src[l->pos+1] == '-') {
        l->pos += 2;
        int start = l->pos;
        while (l->src[l->pos] && !isspace((unsigned char)l->src[l->pos])) l->pos++;
        char buf[64];
        int len = l->pos - start;
        if (len >= (int)sizeof(buf)) len = sizeof(buf)-1;
        strncpy(buf, l->src + start, len);
        buf[len] = '\0';
        if (strcmp(buf, "global") == 0) return make_tok(TOK_FLAG_GLOBAL, "--global", line);
        /* unknown flag — treat as ident for error reporting */
        char fbuf[68]; snprintf(fbuf, sizeof(fbuf), "--%s", buf);
        return make_tok(TOK_UNKNOWN, fbuf, line);
    }

    /* single-char tokens */
    switch (c) {
        case '[': l->pos++; return make_tok(TOK_LBRACKET, "[", line);
        case ']': l->pos++; return make_tok(TOK_RBRACKET, "]", line);
        case '<': l->pos++; return make_tok(TOK_LANGLE,   "<", line);
        case '>': l->pos++; return make_tok(TOK_RANGLE,   ">", line);
        case '(': l->pos++; return make_tok(TOK_LPAREN,   "(", line);
        case ')': l->pos++; return make_tok(TOK_RPAREN,   ")", line);
        case '{': l->pos++; return make_tok(TOK_LBRACE,   "{", line);
        case '}': l->pos++; return make_tok(TOK_RBRACE,   "}", line);
        case '%': l->pos++; return make_tok(TOK_PERCENT,  "%", line);
        case '+': l->pos++; return make_tok(TOK_PLUS,     "+", line);
        case '-': l->pos++; return make_tok(TOK_MINUS,    "-", line);
        case '*': l->pos++; return make_tok(TOK_STAR,     "*", line);
        case '/': l->pos++; return make_tok(TOK_SLASH,    "/", line);
        case '=': l->pos++; return make_tok(TOK_EQUAL,    "=", line);
    }

    /* number */
    if (isdigit((unsigned char)c) || (c == '-' && isdigit((unsigned char)l->src[l->pos+1]))) {
        int start = l->pos;
        if (c == '-') l->pos++;
        while (l->src[l->pos] && (isdigit((unsigned char)l->src[l->pos]) || l->src[l->pos] == '.'))
            l->pos++;
        /* scientific notation: e/E followed by optional +/- and digits */
        if (l->src[l->pos] == 'e' || l->src[l->pos] == 'E') {
            l->pos++;
            if (l->src[l->pos] == '+' || l->src[l->pos] == '-') l->pos++;
            while (l->src[l->pos] && isdigit((unsigned char)l->src[l->pos])) l->pos++;
        }
        int len = l->pos - start;
        char *buf = malloc(len + 1);
        strncpy(buf, l->src + start, len);
        buf[len] = '\0';
        Token t = make_tok(TOK_NUMBER, buf, line);
        free(buf);
        return t;
    }

    /* word (keyword or ident) */
    if (isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
        int start = l->pos;
        while (l->src[l->pos] && (isalnum((unsigned char)l->src[l->pos]) ||
               l->src[l->pos] == '_' || l->src[l->pos] == '?' ||
               (unsigned char)l->src[l->pos] >= 0x80))
            l->pos++;
        int len = l->pos - start;
        char *buf = malloc(len + 1);
        strncpy(buf, l->src + start, len);
        buf[len] = '\0';
        /* keyword check */
        for (int i = 0; KEYWORDS[i].word; i++) {
            if (strcmp(buf, KEYWORDS[i].word) == 0) {
                Token t = make_tok(KEYWORDS[i].type, buf, line);
                free(buf);
                return t;
            }
        }
        Token t = make_tok(TOK_IDENT, buf, line);
        free(buf);
        return t;
    }

    /* unknown */
    char unk[2] = {c, '\0'};
    l->pos++;
    return make_tok(TOK_UNKNOWN, unk, line);
}

Token lexer_peek(Lexer *l) {
    int saved_pos  = l->pos;
    int saved_line = l->line;
    Token t = lexer_next(l);
    l->pos  = saved_pos;
    l->line = saved_line;
    return t;
}

void token_free(Token *t) {
    free(t->value);
    t->value = NULL;
}

const char *token_type_name(TokenType t) {
    switch (t) {
        case TOK_NUMBER:      return "NUMBER";
        case TOK_STRING:      return "STRING";
        case TOK_IDENT:       return "IDENT";
        case TOK_LBRACKET:    return "[";
        case TOK_RBRACKET:    return "]";
        case TOK_LANGLE:      return "<";
        case TOK_RANGLE:      return ">";
        case TOK_LPAREN:      return "(";
        case TOK_RPAREN:      return ")";
        case TOK_LBRACE:      return "{";
        case TOK_RBRACE:      return "}";
        case TOK_PERCENT:     return "%";
        case TOK_PLUS:        return "+";
        case TOK_MINUS:       return "-";
        case TOK_STAR:        return "*";
        case TOK_SLASH:       return "/";
        case TOK_SPRITE:      return "sprite";
        case TOK_STAGE:       return "stage";
        case TOK_WHEN:        return "when";
        case TOK_VAR:         return "var";
        case TOK_LIST:        return "list";
        case TOK_FOREVER:     return "forever";
        case TOK_REPEAT:      return "repeat";
        case TOK_IF:          return "if";
        case TOK_ELSE:        return "else";
        case TOK_NOT:         return "not";
        case TOK_AND:         return "and";
        case TOK_OR:          return "or";
        case TOK_MOD:         return "mod";
        case TOK_TO:          return "to";
        case TOK_OF:          return "of";
        case TOK_BY:          return "by";
        case TOK_FOR:         return "for";
        case TOK_AT:          return "at";
        case TOK_WITH:        return "with";
        case TOK_STEPS:       return "steps";
        case TOK_DEGREES:     return "degrees";
        case TOK_SECONDS:     return "seconds";
        case TOK_SECS:        return "secs";
        case TOK_CONTAINS:    return "contains";
        case TOK_FLAG_GLOBAL: return "--global";
        case TOK_EOF:         return "EOF";
        default:              return "UNKNOWN";
    }
}
