#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

/* Simple proc name registry for call disambiguation */
typedef struct {
    char  *name;
    char **params;
    int    param_count;
} ProcEntry;

typedef struct {
    Lexer      lexer;
    Token      current;
    Token      peeked;
    int        has_peeked;
    int        errors;
    /* custom block registry — populated during sprite parse */
    ProcEntry *procs;
    int        proc_count;
    int        proc_cap;
    /* if inside a define body, these are the in-scope param names */
    char     **cur_params;
    int        cur_param_count;
} Parser;

void     parser_init(Parser *p, const char *src);
Program *parser_parse(Parser *p);

#endif
