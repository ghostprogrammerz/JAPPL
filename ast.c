#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "ast.h"

Expr *expr_new(ExprType type, int line) {
    Expr *e = calloc(1, sizeof(Expr));
    e->type = type;
    e->line = line;
    return e;
}

Stmt *stmt_new(StmtType type, int line) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->type = type;
    s->line = line;
    return s;
}

Script *script_new(HatType hat, const char *hat_arg, int line) {
    Script *s = calloc(1, sizeof(Script));
    s->hat     = hat;
    s->hat_arg = hat_arg ? strdup(hat_arg) : NULL;
    s->line    = line;
    return s;
}

Sprite *sprite_new(const char *name, int is_stage) {
    Sprite *s  = calloc(1, sizeof(Sprite));
    s->name     = name ? strdup(name) : NULL;
    s->is_stage = is_stage;
    return s;
}

Program *program_new(void) {
    return calloc(1, sizeof(Program));
}

/* minimal free — good enough for compiler use */
void program_free(Program *p) {
    free(p);
}
