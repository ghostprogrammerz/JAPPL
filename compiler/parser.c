#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "parser.h"

/* ── helpers ─────────────────────────────────────────────────────── */

static void error(Parser *p, int line, const char *msg) {
    fprintf(stderr, "error at line %d: %s\n", line, msg);
    p->errors++;
}

static void errorf(Parser *p, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "error at line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    p->errors++;
}

static Token advance(Parser *p) {
    if (p->has_peeked) {
        p->has_peeked = 0;
        token_free(&p->current);
        p->current = p->peeked;
    } else {
        token_free(&p->current);
        p->current = lexer_next(&p->lexer);
    }
    return p->current;
}

static Token peek(Parser *p) {
    if (!p->has_peeked) {
        p->peeked     = lexer_next(&p->lexer);
        p->has_peeked = 1;
    }
    return p->peeked;
}

static Token cur(Parser *p) { return p->current; }

static int check(Parser *p, TokenType t) { return cur(p).type == t; }

static Token expect(Parser *p, TokenType t) {
    if (cur(p).type != t) {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected '%s', got '%s'",
                 token_type_name(t),
                 cur(p).value ? cur(p).value : token_type_name(cur(p).type));
        error(p, cur(p).line, buf);
    }
    Token t2 = cur(p);
    t2.value = t2.value ? strdup(t2.value) : NULL;
    advance(p);
    return t2;
}

/* consume ident-like word(s) until a stop token, trim trailing space */
static char *read_name_until(Parser *p, TokenType stop1, TokenType stop2) {
    char buf[256] = {0};
    int  first    = 1;
    while (cur(p).type != stop1 && cur(p).type != stop2
           && cur(p).type != TOK_EOF) {
        /* don't insert a space separator immediately after or before a % or - */
        int no_space = (cur(p).type == TOK_PERCENT) ||
                       (buf[0] && buf[strlen(buf)-1] == '%') ||
                       (cur(p).type == TOK_MINUS) ||
                       (buf[0] && buf[strlen(buf)-1] == '-');
        if (!first && !no_space) strncat(buf, " ", sizeof(buf)-strlen(buf)-1);
        strncat(buf, cur(p).value ? cur(p).value : "", sizeof(buf)-strlen(buf)-1);
        first = 0;
        advance(p);
    }
    int len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len-1])) buf[--len] = '\0';
    return strdup(buf);
}

/* consume a color value (handles # prefix) – concatenates tokens without spaces */
static char *read_color_value(Parser *p) {
    char buf[256] = {0};
    while (1) {
        if (check(p, TOK_UNKNOWN) && cur(p).value && cur(p).value[0] == '#') {
            strncat(buf, cur(p).value, sizeof(buf)-strlen(buf)-1);
            advance(p);
        } else if (check(p, TOK_IDENT) || check(p, TOK_NUMBER)) {
            strncat(buf, cur(p).value, sizeof(buf)-strlen(buf)-1);
            advance(p);
        } else {
            break;
        }
        if (check(p, TOK_RPAREN) || check(p, TOK_EOF) ||
            check(p, TOK_RANGLE) || check(p, TOK_LANGLE) ||
            check(p, TOK_RBRACKET) || check(p, TOK_LBRACKET)) break;
    }
    if (strlen(buf) == 0) return strdup("#000000");
    return strdup(buf);
}

/* consume name for "current" - handles "day of week" specially */
static char *read_current_name(Parser *p) {
    char buf[256] = {0};
    int first = 1;
    while (check(p, TOK_IDENT) || check(p, TOK_OF)) {
        if (!first) strncat(buf, " ", sizeof(buf)-strlen(buf)-1);
        if (check(p, TOK_OF)) {
            strncat(buf, "of", sizeof(buf)-strlen(buf)-1);
            advance(p);
        } else {
            strncat(buf, cur(p).value, sizeof(buf)-strlen(buf)-1);
            advance(p);
        }
        first = 0;
        if (check(p, TOK_RPAREN) || check(p, TOK_EOF) ||
            check(p, TOK_RANGLE) || check(p, TOK_LPAREN) ||
            check(p, TOK_TO)) break;
    }
    if (strlen(buf) == 0) return strdup("year");
    return strdup(buf);
}

/* ── expression parser ───────────────────────────────────────────── */

static Expr *parse_expr(Parser *p);
static Expr *parse_paren_expr(Parser *p);

static int is_structured_expr_start(Parser *p) {
    if (cur(p).type == TOK_NUMBER)   return 1;
    if (cur(p).type == TOK_LBRACKET) return 1;
    if (cur(p).type == TOK_NOT)      return 1;
    if (cur(p).type == TOK_LPAREN)   return 1;
    if (cur(p).type == TOK_LIST)     return 1;
    if (cur(p).type == TOK_MINUS && p->lexer.src[p->lexer.pos] >= '0'
                                  && p->lexer.src[p->lexer.pos] <= '9') return 1;
    if (cur(p).type == TOK_IDENT) {
        const char *kw[] = {"pick","join","letter","length","item","abs","sqrt",
                            "floor","ceiling","sin","cos","tan","round",
                            "exp","exp10","ln","log","key",
                            "mouse","answer","timer","distance",
                            "random","edge","sensing","color","current",
                            "x","y","direction","size","volume","loudness",
                            "username","online","days","costume","backdrop",
                            "touching","item","variable","list",NULL};
        for (int i = 0; kw[i]; i++)
            if (strcmp(cur(p).value, kw[i]) == 0) return 1;
    }
    return 0;
}

static Expr *parse_atom(Parser *p) {
    int line = cur(p).line;

    /* variable (name) */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "variable") == 0) {
        advance(p);
        expect(p, TOK_LPAREN);
        char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = name;
        return e;
    }
    /* legacy [name with spaces] — kept for backward compat */
    if (check(p, TOK_LBRACKET)) {
        /* ([) means the literal string "[" — next token is ) not ] */
        if (peek(p).type == TOK_RPAREN || peek(p).type == TOK_RBRACKET ||
            peek(p).type == TOK_EOF) {
            advance(p);
            Expr *e = expr_new(EXPR_STRING, line);
            e->str = strdup("[");
            return e;
        }
        advance(p);
        char *name = read_name_until(p, TOK_RBRACKET, TOK_EOF);
        expect(p, TOK_RBRACKET);
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = name;
        return e;
    }

    /* number */
    if (check(p, TOK_NUMBER)) {
        Expr *e = expr_new(EXPR_NUMBER, line);
        e->number = atof(cur(p).value);
        advance(p);
        return e;
    }

    /* handle negative number in parentheses */
    if (check(p, TOK_MINUS) && peek(p).type == TOK_NUMBER) {
        advance(p);
        Expr *e = expr_new(EXPR_NUMBER, line);
        e->number = -atof(cur(p).value);
        advance(p);
        return e;
    }

    /* not (expr) */
    if (check(p, TOK_NOT)) {
        advance(p);
        expect(p, TOK_LPAREN);
        Expr *inner = parse_expr(p);
        expect(p, TOK_RPAREN);
        Expr *e = expr_new(EXPR_NOT, line);
        e->unary.expr = inner;
        return e;
    }

    /* pick random (a) to (b) */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "pick") == 0) {
        Token nx = peek(p);
        if (!(nx.type == TOK_IDENT && strcmp(nx.value, "random") == 0)) {
            Expr *e = expr_new(EXPR_VAR, line);
            e->str = strdup(cur(p).value);
            advance(p);
            return e;
        }
        advance(p);
        if (!check(p, TOK_IDENT) || strcmp(cur(p).value, "random") != 0) {
            error(p, line, "expected 'random' after 'pick'");
            return expr_new(EXPR_NUMBER, line);
        }
        advance(p);
        expect(p, TOK_LPAREN);
        Expr *a = parse_expr(p);
        expect(p, TOK_RPAREN);
        expect(p, TOK_TO);
        expect(p, TOK_LPAREN);
        Expr *b = parse_expr(p);
        expect(p, TOK_RPAREN);
        Expr *e = expr_new(EXPR_PICK_RANDOM, line);
        e->pair.a = a; e->pair.b = b;
        return e;
    }

    /* join((a) + (b)) new style, or join (a) (b) old style */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "join") == 0) {
        advance(p);
        /* new style: join((a) + (b)) — single paren containing a binop */
        if (check(p, TOK_LPAREN)) {
            Token nx = peek(p);
            /* peek inside: if after '(' we see '(' (structured start), it might be new style */
            /* parse one paren_expr and check if result is a BINOP(+) */
            Expr *first = parse_paren_expr(p);
            if (first->type == EXPR_BINOP && strcmp(first->binop.op, "+") == 0) {
                /* convert top-level + chain into nested JOINs */
                Expr *e = expr_new(EXPR_JOIN, line);
                e->pair.a = first->binop.left;
                e->pair.b = first->binop.right;
                free(first);
                /* handle more chained + args */
                while (check(p, TOK_LPAREN) || is_structured_expr_start(p)) {
                    Expr *next = parse_paren_expr(p);
                    if (next->type == EXPR_BINOP && strcmp(next->binop.op, "+") == 0) {
                        Expr *inner = expr_new(EXPR_JOIN, line);
                        inner->pair.a = next->binop.left;
                        inner->pair.b = next->binop.right;
                        free(next);
                        next = inner;
                    }
                    Expr *new_e = expr_new(EXPR_JOIN, line);
                    new_e->pair.a = e;
                    new_e->pair.b = next;
                    e = new_e;
                }
                return e;
            }
            /* old style: first arg already parsed, get second */
            Expr *right = parse_paren_expr(p);
            Expr *e = expr_new(EXPR_JOIN, line);
            e->pair.a = first; e->pair.b = right;
            while (check(p, TOK_LPAREN) || is_structured_expr_start(p)) {
                Expr *next = parse_paren_expr(p);
                Expr *new_e = expr_new(EXPR_JOIN, line);
                new_e->pair.a = e;
                new_e->pair.b = next;
                e = new_e;
            }
            return e;
        }
        /* no paren at all — error fallback */
        Expr *e = expr_new(EXPR_JOIN, line);
        e->pair.a = expr_new(EXPR_STRING, line);
        e->pair.a->str = strdup("");
        e->pair.b = expr_new(EXPR_STRING, line);
        e->pair.b->str = strdup("");
        return e;
    }

    /* letter (n) of (str) */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "letter") == 0) {
        advance(p);
        expect(p, TOK_LPAREN);
        Expr *n = parse_expr(p);
        expect(p, TOK_RPAREN);
        expect(p, TOK_OF);
        Expr *s;
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "answer") == 0) {
            int ln2 = cur(p).line;
            advance(p);
            s = expr_new(EXPR_ANSWER, ln2);
        } else if (check(p, TOK_IDENT)) {
            s = parse_expr(p);
        } else {
            expect(p, TOK_LPAREN);
            s = parse_expr(p);
            expect(p, TOK_RPAREN);
        }
        Expr *e = expr_new(EXPR_LETTER_OF, line);
        e->letter_of.n = n;
        e->letter_of.str = s;
        return e;
    }

    /* length of (str) or length of <list> */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "length") == 0) {
        Token nx = peek(p);
        if (nx.type != TOK_OF && !(nx.type==TOK_IDENT && strcmp(nx.value,"of")==0)) {
            Expr *e = expr_new(EXPR_VAR, line);
            e->str = strdup(cur(p).value);
            advance(p);
            return e;
        }
        advance(p);
        expect(p, TOK_OF);
        if (check(p, TOK_LIST)) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Expr *e = expr_new(EXPR_LIST_LENGTH, line);
            e->list_len.list = strdup(name);
            return e;
        }
        if (check(p, TOK_LANGLE)) {
            advance(p);
            char *name = read_name_until(p, TOK_RANGLE, TOK_EOF);
            expect(p, TOK_RANGLE);
            Expr *e = expr_new(EXPR_LIST_LENGTH, line);
            e->list_len.list = strdup(name);
            return e;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "answer") == 0) {
            int ln2 = cur(p).line;
            advance(p);
            Expr *ans = expr_new(EXPR_ANSWER, ln2);
            Expr *e = expr_new(EXPR_LENGTH_OF, line);
            e->unary.expr = ans;
            return e;
        }
        if (check(p, TOK_IDENT)) {
            Expr *s = parse_expr(p);
            Expr *e = expr_new(EXPR_LENGTH_OF, line);
            e->unary.expr = s;
            return e;
        }
        expect(p, TOK_LPAREN);
        Expr *s = parse_expr(p);
        expect(p, TOK_RPAREN);
        Expr *e = expr_new(EXPR_LENGTH_OF, line);
        e->unary.expr = s;
        return e;
    }

    /* item (n) of <list> */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "item") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "num") == 0) {
            advance(p); advance(p);
            expect(p, TOK_LPAREN);
            Expr *val = parse_expr(p);
            expect(p, TOK_RPAREN);
            expect(p, TOK_OF);
            if (check(p, TOK_LIST)) advance(p);
            if (check(p, TOK_LPAREN)) {
                advance(p);
                char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
                Expr *e = expr_new(EXPR_LIST_ITEM_NUM, line);
                e->list_item.list = strdup(name); e->list_item.index = val; return e;
            }
            expect(p, TOK_LANGLE);
            char *name = read_name_until(p, TOK_RANGLE, TOK_EOF); expect(p, TOK_RANGLE);
            Expr *e = expr_new(EXPR_LIST_ITEM_NUM, line);
            e->list_item.list = strdup(name); e->list_item.index = val; return e;
        }
        advance(p);
        expect(p, TOK_LPAREN);
        Expr *idx = parse_expr(p);
        expect(p, TOK_RPAREN);
        expect(p, TOK_OF);
        if (check(p, TOK_LIST)) advance(p);
        if (check(p, TOK_LPAREN)) {
            advance(p);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
            Expr *e = expr_new(EXPR_LIST_ITEM, line);
            e->list_item.list = strdup(name); e->list_item.index = idx; return e;
        }
        expect(p, TOK_LANGLE);
        char *name = read_name_until(p, TOK_RANGLE, TOK_EOF); expect(p, TOK_RANGLE);
        Expr *e = expr_new(EXPR_LIST_ITEM, line);
        e->list_item.list = strdup(name); e->list_item.index = idx; return e;
    }

    /* math functions */
    if (check(p, TOK_IDENT)) {
        const char *fns[] = {"abs","sqrt","floor","ceiling","sin","cos","tan","round",
                              "exp","exp10","ln","log",NULL};
        for (int i = 0; fns[i]; i++) {
            if (strcmp(cur(p).value, fns[i]) == 0) {
                char *fn = strdup(cur(p).value);
                advance(p);
                expect(p, TOK_LPAREN);
                Expr *arg = parse_expr(p);
                expect(p, TOK_RPAREN);
                ExprType et = strcmp(fn,"round")==0 ? EXPR_ROUND : EXPR_MATH_FN;
                Expr *e = expr_new(et, line);
                if (et == EXPR_MATH_FN) { e->mathfn.fn = fn; e->mathfn.arg = arg; }
                else { e->unary.expr = arg; free(fn); }
                return e;
            }
        }
    }

    /* sensing reporters */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "key") == 0) {
        advance(p);
        expect(p, TOK_LPAREN);
        char *key = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "pressed") == 0) advance(p);
        Expr *e = expr_new(EXPR_KEY_PRESSED, line);
        e->str = key;
        return e;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "mouse") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "x") == 0) { advance(p); return expr_new(EXPR_MOUSE_X, line); }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "y") == 0) { advance(p); return expr_new(EXPR_MOUSE_Y, line); }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "down") == 0) { advance(p); return expr_new(EXPR_MOUSE_DOWN, line); }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "pointer") == 0) { advance(p); return expr_new(EXPR_MOUSE_POINTER, line); }
        error(p, line, "unknown 'mouse' expression");
        return expr_new(EXPR_NUMBER, line);
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "answer") == 0)  { advance(p); return expr_new(EXPR_ANSWER, line); }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "timer") == 0)   { advance(p); return expr_new(EXPR_TIMER,  line); }

    /* ── touching color (#rrggbb) – must come BEFORE the simple touching handler ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "touching") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "color") == 0) {
            advance(p); advance(p);
            expect(p, TOK_LPAREN);
            char *col = read_color_value(p);
            expect(p, TOK_RPAREN);
            Expr *e = expr_new(EXPR_TOUCHING_COLOR, line);
            e->str = col;
            return e;
        }
    }

    /* simple touching (target) – must be after touching color */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "touching") == 0) {
        advance(p);
        expect(p, TOK_LPAREN);
        char *tgt = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        Expr *e = expr_new(EXPR_TOUCHING, line);
        e->touching.target = tgt;
        return e;
    }

    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "distance") == 0) {
        advance(p);
        expect(p, TOK_TO);
        expect(p, TOK_LPAREN);
        char *tgt = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        Expr *e = expr_new(EXPR_DISTANCE_TO, line);
        e->touching.target = tgt;
        return e;
    }

    /* random position / mouse pointer / edge as bare atoms */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "random") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "position") == 0) {
            advance(p); advance(p);
            return expr_new(EXPR_RANDOM_POSITION, line);
        }
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "edge") == 0) {
        advance(p); return expr_new(EXPR_EDGE, line);
    }

    /* parenthesized sub-expression */
    if (check(p, TOK_LPAREN)) {
        return parse_paren_expr(p);
    }

    /* ── x position / y position ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "x") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "position") == 0) {
            advance(p); advance(p);
            return expr_new(EXPR_X_POS, line);
        }
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = strdup("x");
        advance(p);
        return e;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "y") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "position") == 0) {
            advance(p); advance(p);
            return expr_new(EXPR_Y_POS, line);
        }
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = strdup("y");
        advance(p);
        return e;
    }

    /* ── zero-arg reporters ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "direction") == 0)
        { advance(p); return expr_new(EXPR_DIRECTION, line); }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "size") == 0)
        { advance(p); return expr_new(EXPR_SIZE, line); }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "volume") == 0)
        { advance(p); return expr_new(EXPR_VOLUME, line); }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "loudness") == 0)
        { advance(p); return expr_new(EXPR_LOUDNESS, line); }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "username") == 0)
        { advance(p); return expr_new(EXPR_USERNAME, line); }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "online") == 0)
        { advance(p); return expr_new(EXPR_ONLINE, line); }

    /* ── days since 2000 ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "days") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "since") == 0) {
            advance(p); advance(p);
            if (check(p, TOK_NUMBER)) advance(p);
            return expr_new(EXPR_DAYS_SINCE_2000, line);
        }
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = strdup("days");
        advance(p);
        return e;
    }

    /* ── current (year/month/date/etc) ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "current") == 0) {
        advance(p);
        char *which = NULL;
        if (check(p, TOK_LPAREN)) {
            advance(p);
            which = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
        } else {
            which = read_current_name(p);
        }
        Expr *e = expr_new(EXPR_CURRENT, line);
        e->str = which;
        return e;
    }

    /* ── costume number/name ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "costume") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "number") == 0)
            { advance(p); advance(p); return expr_new(EXPR_COSTUME_NUM, line); }
        if (nx.type == TOK_IDENT && strcmp(nx.value, "name") == 0)
            { advance(p); advance(p); return expr_new(EXPR_COSTUME_NAME, line); }
    }

    /* ── backdrop number/name ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "backdrop") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "number") == 0)
            { advance(p); advance(p); return expr_new(EXPR_BACKDROP_NUM, line); }
        if (nx.type == TOK_IDENT && strcmp(nx.value, "name") == 0)
            { advance(p); advance(p); return expr_new(EXPR_BACKDROP_NAME, line); }
    }

    /* ── color touching color (#c1) (#c2) ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "color") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_LPAREN) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *c1 = read_color_value(p);
            expect(p, TOK_RPAREN);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "touching") == 0) {
                advance(p);
                expect(p, TOK_LPAREN);
                char *c2 = read_color_value(p);
                expect(p, TOK_RPAREN);
                Expr *e  = expr_new(EXPR_COLOR_TOUCHING_COLOR, line);
                Expr *ea = expr_new(EXPR_STRING, line);
                ea->str = c1;
                Expr *eb = expr_new(EXPR_STRING, line);
                eb->str = c2;
                e->pair.a = ea;
                e->pair.b = eb;
                return e;
            }
            free(c1);
        }
        /* If not followed by '(', treat as variable */
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = strdup("color");
        advance(p);
        return e;
    }

    /* ── sensing (sprite) (property) ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "sensing") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_LPAREN) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *sprite = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            expect(p, TOK_LPAREN);
            char *prop = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Expr *e = expr_new(EXPR_SENSING_OF, line);
            e->sensing_of.sprite   = sprite;
            e->sensing_of.property = prop;
            return e;
        }
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = strdup("sensing");
        advance(p);
        return e;
    }

    /* list (name) contains (x) */
    if (check(p, TOK_LIST)) {
        advance(p);
        expect(p, TOK_LPAREN);
        char *lname = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        if (check(p, TOK_CONTAINS)) {
            advance(p);
            Expr *item = parse_paren_expr(p);
            Expr *e = expr_new(EXPR_LIST_LENGTH, line); /* reuse as placeholder */
            /* Actually use LIST_CONTAINS */
            free(e);
            e = expr_new(EXPR_LIST_CONTAINS, line);
            e->list_contains.list = lname;
            e->list_contains.val  = item;
            return e;
        }
        /* bare list (name) — return as string */
        Expr *e = expr_new(EXPR_STRING, line);
        e->str = lname;
        return e;
    }

    /* fallthrough: treat bare ident as string or arg reporter */
    if (check(p, TOK_IDENT)) {
        if (p->cur_param_count > 0) {
            for (int i = 0; i < p->cur_param_count; i++) {
                if (strcmp(cur(p).value, p->cur_params[i]) == 0) {
                    Expr *e = expr_new(EXPR_ARG_REPORTER, line);
                    e->str = strdup(cur(p).value);
                    advance(p);
                    return e;
                }
            }
        }
        Expr *e = expr_new(EXPR_VAR, line);
        e->str = strdup(cur(p).value);
        advance(p);
        return e;
    }

    errorf(p, line, "unexpected token '%s' in expression",
           cur(p).value ? cur(p).value : token_type_name(cur(p).type));
    advance(p);
    return expr_new(EXPR_NUMBER, line);
}

static int is_binop(Parser *p, char *op_out) {
    switch (cur(p).type) {
        case TOK_PLUS:  strcpy(op_out, "+");   return 1;
        case TOK_MINUS: strcpy(op_out, "-");   return 1;
        case TOK_STAR:  strcpy(op_out, "*");   return 1;
        case TOK_SLASH: strcpy(op_out, "/");   return 1;
        case TOK_MOD:    strcpy(op_out, "mod"); return 1;
        case TOK_AND:    strcpy(op_out, "and"); return 1;
        case TOK_OR:     strcpy(op_out, "or");  return 1;
        case TOK_EQUAL:  strcpy(op_out, "=");   return 1;
        case TOK_RANGLE: strcpy(op_out, ">");   return 1;
        case TOK_LANGLE: strcpy(op_out, "<");   return 1;
        default: break;
    }
    return 0;
}

/* Apply a trailing 'contains (sub)' suffix to an expression if present.
   Factors out the repeated pattern so it works both at top-level and on
   the RHS of a binop (fixes Bugs 4 & 5). */
static Expr *maybe_contains(Parser *p, Expr *left) {
    if (!check(p, TOK_CONTAINS)) return left;
    int line = cur(p).line;
    advance(p);
    expect(p, TOK_LPAREN);
    Expr *sub = parse_expr(p);
    expect(p, TOK_RPAREN);
    Expr *e = expr_new(EXPR_STR_CONTAINS, line);
    e->str_contains.str = left;
    e->str_contains.sub = sub;
    return e;
}

static Expr *parse_expr(Parser *p) {
    Expr *left = maybe_contains(p, parse_atom(p));
    char op[8];
    while (is_binop(p, op)) {
        int line = cur(p).line;
        advance(p);
        /* Apply contains on the RHS atom before wrapping in binop (Bug 4 & 5) */
        Expr *right = maybe_contains(p, parse_atom(p));
        Expr *e = expr_new(EXPR_BINOP, line);
        e->binop.left  = left;
        e->binop.right = right;
        strcpy(e->binop.op, op);
        left = e;
    }
    /* Also apply contains on the fully-assembled left side (handles top-level) */
    return maybe_contains(p, left);
}

static Expr *parse_paren_expr(Parser *p) {
    if (is_structured_expr_start(p) && cur(p).type != TOK_LPAREN) {
        return parse_expr(p);
    }
    expect(p, TOK_LPAREN);
    int line = cur(p).line;

    /* Bug 6: (( )) — Scratch empty-string slot. The outer '(' was consumed
       above; if we now see '(' immediately followed by ')', that is the
       empty-string pattern — return an empty EXPR_STRING instead of recursing
       and miscounting parens. */
    if (check(p, TOK_LPAREN) && peek(p).type == TOK_RPAREN) {
        advance(p); /* consume inner '(' */
        advance(p); /* consume inner ')' */
        expect(p, TOK_RPAREN); /* consume outer ')' */
        Expr *e = expr_new(EXPR_STRING, line);
        e->str = strdup("");
        return e;
    }

    /* Param check MUST come before is_structured_expr_start so that a param
       named with a keyword token (e.g. "list ID") is caught as EXPR_ARG_REPORTER
       instead of being parsed as a list/keyword expression. */
    if (p->cur_param_count > 0) {
        int saved_pos        = p->lexer.pos;
        int saved_lex_line   = p->lexer.line;
        Token saved_cur      = p->current;
        if (saved_cur.value) saved_cur.value = strdup(saved_cur.value);
        int saved_has_peeked = p->has_peeked;
        Token saved_peeked   = p->peeked;
        if (saved_has_peeked && saved_peeked.value) saved_peeked.value = strdup(saved_peeked.value);

        char buf[256] = {0};
        int first = 1;
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            if (!first) strncat(buf, " ", sizeof(buf)-strlen(buf)-1);
            strncat(buf, cur(p).value ? cur(p).value : "", sizeof(buf)-strlen(buf)-1);
            first = 0;
            advance(p);
        }
        if (check(p, TOK_RPAREN)) {
            for (int i = 0; i < p->cur_param_count; i++) {
                if (strcmp(buf, p->cur_params[i]) == 0) {
                    Expr *e = expr_new(EXPR_ARG_REPORTER, line);
                    e->str = strdup(buf);
                    expect(p, TOK_RPAREN);
                    free(saved_cur.value);
                    if (saved_has_peeked) free(saved_peeked.value);
                    return e;
                }
            }
        }
        /* no param match — rewind */
        p->lexer.pos  = saved_pos;
        p->lexer.line = saved_lex_line;
        token_free(&p->current);
        p->current    = saved_cur;
        p->has_peeked = saved_has_peeked;
        if (p->has_peeked) {
            token_free(&p->peeked);
            p->peeked = saved_peeked;
        }
    }

    /* Bug 5: if the only reason we'd take the structured path is a leading
       TOK_NUMBER, but the next token is NOT ')' (e.g. "0123456789." where '.'
       follows the number), fall through to raw-string concat so the whole
       digit-plus-punctuation sequence is captured as a string literal. */
    int force_raw_string = 0;
    if (cur(p).type == TOK_NUMBER) {
        Token nx = peek(p);
        if (nx.type != TOK_RPAREN && !is_binop(p, (char[8]){})) {
            /* Check: after the number, if we have something that isn't a
               closing paren or a binop operator, it's a raw string like
               "0123456789." — don't parse as a number expression. */
            int nx_is_binop = (nx.type == TOK_PLUS || nx.type == TOK_MINUS ||
                               nx.type == TOK_STAR || nx.type == TOK_SLASH ||
                               nx.type == TOK_MOD  || nx.type == TOK_AND   ||
                               nx.type == TOK_OR   || nx.type == TOK_EQUAL ||
                               nx.type == TOK_RANGLE || nx.type == TOK_LANGLE);
            if (!nx_is_binop) force_raw_string = 1;
        }
    }
    if (!force_raw_string && is_structured_expr_start(p)) {
        Expr *e = parse_expr(p);
        e = maybe_contains(p, e);
        expect(p, TOK_RPAREN);
        return e;
    }

    if (p->cur_param_count > 0) {
        /* already handled above, before structured-expr check */
    }

    char buf2[256] = {0};
    int first2 = 1;
    int token_count = 0;
    int first_is_ident = (cur(p).type == TOK_IDENT);
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        if (!first2) strncat(buf2, " ", sizeof(buf2)-strlen(buf2)-1);
        strncat(buf2, cur(p).value ? cur(p).value : "", sizeof(buf2)-strlen(buf2)-1);
        first2 = 0; token_count++;
        advance(p);
    }
    expect(p, TOK_RPAREN);
    Expr *e;
    /* treat any all-ident multi-word content as a variable name,
       not a string literal — e.g. (list ID), (dot result), (pick?) */
    if (first_is_ident) {
        e = expr_new(EXPR_VAR, line);
    } else {
        e = expr_new(EXPR_STRING, line);
    }
    e->str = strdup(buf2);
    /* handle (X) contains (Y) suffix */
    if (check(p, TOK_CONTAINS)) {
        int cline = cur(p).line;
        advance(p);
        Expr *sub = parse_paren_expr(p);
        Expr *ce = expr_new(EXPR_STR_CONTAINS, cline);
        ce->str_contains.str = e;
        ce->str_contains.sub = sub;
        return ce;
    }
    return e;
}

/* ── statement body ──────────────────────────────────────────────── */

static Stmt *parse_stmt(Parser *p);

static void parse_body(Parser *p, Stmt ***body_out, int *count_out) {
    expect(p, TOK_LBRACE);
    Stmt **body = NULL;
    int   count = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Stmt *s = parse_stmt(p);
        if (s) {
            body = realloc(body, (count+1) * sizeof(Stmt*));
            body[count++] = s;
        }
    }
    expect(p, TOK_RBRACE);
    *body_out  = body;
    *count_out = count;
}

/* ── statement parser ────────────────────────────────────────────── */

static Stmt *parse_stmt(Parser *p) {
    int line = cur(p).line;

    /* ── control ── */
    if (check(p, TOK_FOREVER)) {
        advance(p);
        Stmt *s = stmt_new(STMT_FOREVER, line);
        parse_body(p, &s->body, &s->body_count);
        return s;
    }
    if (check(p, TOK_REPEAT)) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "until") == 0) {
            advance(p);
            Stmt *s = stmt_new(STMT_REPEAT_UNTIL, line);
            s->cond = parse_paren_expr(p);
            parse_body(p, &s->body, &s->body_count);
            return s;
        }
        Stmt *s = stmt_new(STMT_REPEAT, line);
        s->count = parse_paren_expr(p);
        parse_body(p, &s->body, &s->body_count);
        return s;
    }
    /* ── if on edge bounce — MUST come before regular if ── */
    if (check(p, TOK_IF)) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "on") == 0) {
            advance(p); advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "edge") == 0) advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "bounce") == 0) advance(p);
            return stmt_new(STMT_IF_ON_EDGE_BOUNCE, line);
        }
    }
    if (check(p, TOK_IF)) {
        advance(p);
        Expr *cond = parse_paren_expr(p);
        Stmt **body; int bc;
        parse_body(p, &body, &bc);
        if (check(p, TOK_ELSE)) {
            advance(p);
            Stmt **ebody; int ec;
            parse_body(p, &ebody, &ec);
            Stmt *s = stmt_new(STMT_IF_ELSE, line);
            s->cond = cond;
            s->body = body; s->body_count = bc;
            s->else_body = ebody; s->else_count = ec;
            return s;
        }
        Stmt *s = stmt_new(STMT_IF, line);
        s->cond = cond; s->body = body; s->body_count = bc;
        return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "wait") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "until") == 0) {
            advance(p);
            Stmt *s = stmt_new(STMT_WAIT_UNTIL, line);
            s->cond = parse_paren_expr(p);
            return s;
        }
        Stmt *s = stmt_new(STMT_WAIT, line);
        s->secs = parse_paren_expr(p);
        return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "stop") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "all") == 0) {
            Token nx = peek(p);
            if (nx.type == TOK_IDENT && strcmp(nx.value, "sounds") == 0) {
                advance(p); advance(p);
                return stmt_new(STMT_STOP_SOUNDS, line);
            }
        }
        Stmt *s = stmt_new(STMT_STOP, line);
        char buf[64] = {0};
        int first = 1;
        static const char *stop_words[] = {"all","this","script","other","scripts","in","sprite",NULL};
        while (check(p, TOK_IDENT) || check(p, TOK_SPRITE)) {
            int known = 0;
            for (int i = 0; stop_words[i]; i++)
                if (strcmp(cur(p).value, stop_words[i]) == 0) { known = 1; break; }
            if (check(p, TOK_SPRITE)) known = 1;
            if (!known) break;
            if (!first) strncat(buf, " ", sizeof(buf)-strlen(buf)-1);
            strncat(buf, cur(p).value, sizeof(buf)-strlen(buf)-1);
            first = 0;
            advance(p);
        }
        s->name = strdup(buf);
        return s;
    }

    /* ── motion ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "move") == 0) {
        advance(p);
        Stmt *s = stmt_new(STMT_MOVE_STEPS, line);
        s->a = parse_paren_expr(p);
        if (check(p, TOK_STEPS)) advance(p);
        return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "turn") == 0) {
        advance(p);
        int right = 1;
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "left") == 0)  { right = 0; advance(p); }
        else if (check(p, TOK_IDENT) && strcmp(cur(p).value, "right") == 0) advance(p);
        Stmt *s = stmt_new(right ? STMT_TURN_RIGHT : STMT_TURN_LEFT, line);
        s->a = parse_paren_expr(p);
        if (check(p, TOK_DEGREES)) advance(p);
        return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "go") == 0) {
        advance(p);
        if (check(p, TOK_TO)) {
            advance(p);
            if (check(p, TOK_IDENT) && (strcmp(cur(p).value, "front") == 0 || strcmp(cur(p).value, "back") == 0)) {
                char *pos = strdup(cur(p).value);
                advance(p);
                if (check(p, TOK_IDENT) && strcmp(cur(p).value, "layer") == 0) {
                    advance(p);
                    Stmt *s = stmt_new(STMT_GOTO_FRONT_BACK, line);
                    s->name = pos;
                    return s;
                }
                free(pos);
                error(p, line, "expected 'layer' after front/back");
                return NULL;
            }
            expect(p, TOK_LPAREN);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "random") == 0) {
                Token nx = peek(p);
                if (nx.type == TOK_IDENT && strcmp(nx.value, "position") == 0) {
                    advance(p); advance(p);
                    expect(p, TOK_RPAREN);
                    Stmt *s = stmt_new(STMT_GOTO_TARGET, line);
                    s->target = strdup("random position");
                    return s;
                }
            }
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "mouse") == 0) {
                Token nx = peek(p);
                if (nx.type == TOK_IDENT && strcmp(nx.value, "pointer") == 0) {
                    advance(p); advance(p);
                    expect(p, TOK_RPAREN);
                    Stmt *s = stmt_new(STMT_GOTO_TARGET, line);
                    s->target = strdup("mouse pointer");
                    return s;
                }
            }
            Expr *x = parse_expr(p);
            expect(p, TOK_RPAREN);
            Expr *y = parse_paren_expr(p);
            Stmt *s = stmt_new(STMT_GOTO_XY, line);
            s->a = x; s->b = y;
            return s;
        }
        if (check(p, TOK_IDENT) && (strcmp(cur(p).value, "forward") == 0 || strcmp(cur(p).value, "backward") == 0)) {
            char *dir = strdup(cur(p).value);
            advance(p);
            Expr *num = parse_paren_expr(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "layers") == 0) {
                advance(p);
                Stmt *s = stmt_new(STMT_GOTO_LAYER, line);
                s->name = dir;
                s->a = num;
                return s;
            }
            free(dir);
        }
        error(p, line, "unknown 'go' statement");
        return NULL;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "glide") == 0) {
        advance(p);
        Expr *secs = parse_paren_expr(p);
        if (check(p, TOK_SECS)) advance(p);
        expect(p, TOK_TO);
        expect(p, TOK_LPAREN);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "random") == 0) {
            Token nx = peek(p);
            if (nx.type == TOK_IDENT && strcmp(nx.value, "position") == 0) {
                advance(p); advance(p); expect(p, TOK_RPAREN);
                Stmt *s = stmt_new(STMT_GLIDE_TARGET, line);
                s->secs = secs; s->target = strdup("random position");
                return s;
            }
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "mouse") == 0) {
            Token nx = peek(p);
            if (nx.type == TOK_IDENT && strcmp(nx.value, "pointer") == 0) {
                advance(p); advance(p); expect(p, TOK_RPAREN);
                Stmt *s = stmt_new(STMT_GLIDE_TARGET, line);
                s->secs = secs; s->target = strdup("mouse pointer");
                return s;
            }
        }
        Expr *x = parse_expr(p); expect(p, TOK_RPAREN);
        Expr *y = parse_paren_expr(p);
        Stmt *s = stmt_new(STMT_GLIDE_XY, line);
        s->secs = secs; s->a = x; s->b = y;
        return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "set") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "x") == 0) {
            advance(p); expect(p, TOK_TO);
            Stmt *s = stmt_new(STMT_SET_X, line); s->a = parse_paren_expr(p); return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "y") == 0) {
            advance(p); expect(p, TOK_TO);
            Stmt *s = stmt_new(STMT_SET_Y, line); s->a = parse_paren_expr(p); return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "size") == 0) {
            advance(p); expect(p, TOK_TO);
            Stmt *s = stmt_new(STMT_SET_SIZE, line); s->a = parse_paren_expr(p);
            if (check(p, TOK_PERCENT)) advance(p);
            return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "volume") == 0) {
            advance(p); expect(p, TOK_TO);
            Stmt *s = stmt_new(STMT_SET_VOLUME, line); s->a = parse_paren_expr(p);
            if (check(p, TOK_PERCENT)) advance(p);
            return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "variable") == 0) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            expect(p, TOK_TO);
            Stmt *s = stmt_new(STMT_SET_VAR, line);
            s->name = name;
            s->a = parse_paren_expr(p);
            return s;
        }
        /* legacy set [name] to */
        if (check(p, TOK_LBRACKET)) {
            advance(p);
            char *name = read_name_until(p, TOK_RBRACKET, TOK_EOF);
            expect(p, TOK_RBRACKET);
            expect(p, TOK_TO);
            Stmt *s = stmt_new(STMT_SET_VAR, line);
            s->name = name;
            if (check(p, TOK_LPAREN))
                s->a = parse_paren_expr(p);
            else
                s->a = parse_expr(p);
            return s;
        }
        if (check(p, TOK_IDENT)) {
            if (strcmp(cur(p).value, "rotation") == 0) {
                advance(p);
                if (check(p, TOK_IDENT) && strcmp(cur(p).value, "style") == 0) advance(p);
                expect(p, TOK_TO);
                expect(p, TOK_LPAREN);
                char *style = read_name_until(p, TOK_RPAREN, TOK_EOF);
                expect(p, TOK_RPAREN);
                Stmt *s = stmt_new(STMT_SET_ROTATION_STYLE, line);
                s->name = style;
                return s;
            }
            if (strcmp(cur(p).value, "drag") == 0) {
                advance(p);
                if (check(p, TOK_IDENT) && strcmp(cur(p).value, "mode") == 0) advance(p);
                expect(p, TOK_TO);
                expect(p, TOK_LPAREN);
                char *mode = read_name_until(p, TOK_RPAREN, TOK_EOF);
                expect(p, TOK_RPAREN);
                Stmt *s = stmt_new(STMT_SET_DRAG_MODE, line);
                s->name = mode;
                return s;
            }
            const char *effects[] = {"color","fisheye","whirl","pixelate","mosaic","brightness","ghost",NULL};
            for (int i = 0; effects[i]; i++) {
                if (strcmp(cur(p).value, effects[i]) == 0) {
                    char *effect = strdup(cur(p).value);
                    advance(p);
                    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "effect") == 0) {
                        advance(p);
                        expect(p, TOK_TO);
                        Stmt *s = stmt_new(STMT_SET_EFFECT, line);
                        s->name = effect;
                        s->a = parse_paren_expr(p);
                        return s;
                    }
                    free(effect);
                    break;
                }
            }
            /* sound effects: pitch / pan */
            const char *seffects[] = {"pitch","pan",NULL};
            for (int i = 0; seffects[i]; i++) {
                if (strcmp(cur(p).value, seffects[i]) == 0) {
                    char *effect = strdup(cur(p).value);
                    advance(p);
                    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "effect") == 0) advance(p);
                    expect(p, TOK_TO);
                    Stmt *s = stmt_new(STMT_SET_SOUND_EFFECT, line);
                    s->name = effect;
                    s->a = parse_paren_expr(p);
                    return s;
                }
            }
        }
        error(p, line, "unknown 'set' statement");
        return NULL;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "change") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "x") == 0) {
            advance(p); expect(p, TOK_BY);
            Stmt *s = stmt_new(STMT_CHANGE_X, line); s->a = parse_paren_expr(p); return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "y") == 0) {
            advance(p); expect(p, TOK_BY);
            Stmt *s = stmt_new(STMT_CHANGE_Y, line); s->a = parse_paren_expr(p); return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "size") == 0) {
            advance(p); expect(p, TOK_BY);
            Stmt *s = stmt_new(STMT_CHANGE_SIZE, line); s->a = parse_paren_expr(p); return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "volume") == 0) {
            advance(p); expect(p, TOK_BY);
            Stmt *s = stmt_new(STMT_CHANGE_VOLUME, line); s->a = parse_paren_expr(p); return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "variable") == 0) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            expect(p, TOK_BY);
            Stmt *s = stmt_new(STMT_CHANGE_VAR, line);
            s->name = name;
            s->a = parse_paren_expr(p);
            return s;
        }
        /* legacy change [name] by */
        if (check(p, TOK_LBRACKET)) {
            advance(p);
            char *name = read_name_until(p, TOK_RBRACKET, TOK_EOF);
            expect(p, TOK_RBRACKET);
            expect(p, TOK_BY);
            Stmt *s = stmt_new(STMT_CHANGE_VAR, line);
            s->name = name;
            s->a = parse_paren_expr(p);
            return s;
        }
        if (check(p, TOK_IDENT)) {
            const char *effects[] = {"color","fisheye","whirl","pixelate","mosaic","brightness","ghost",NULL};
            for (int i = 0; effects[i]; i++) {
                if (strcmp(cur(p).value, effects[i]) == 0) {
                    char *effect = strdup(cur(p).value);
                    advance(p);
                    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "effect") == 0) {
                        advance(p);
                        expect(p, TOK_BY);
                        Stmt *s = stmt_new(STMT_CHANGE_EFFECT, line);
                        s->name = effect;
                        s->a = parse_paren_expr(p);
                        return s;
                    }
                    free(effect);
                    break;
                }
            }
            /* sound effects: pitch / pan */
            const char *seffects[] = {"pitch","pan",NULL};
            for (int i = 0; seffects[i]; i++) {
                if (strcmp(cur(p).value, seffects[i]) == 0) {
                    char *effect = strdup(cur(p).value);
                    advance(p);
                    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "effect") == 0) advance(p);
                    expect(p, TOK_BY);
                    Stmt *s = stmt_new(STMT_CHANGE_SOUND_EFFECT, line);
                    s->name = effect;
                    s->a = parse_paren_expr(p);
                    return s;
                }
            }
        }
        error(p, line, "unknown 'change' statement");
        return NULL;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "point") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "in") == 0) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "direction") == 0) advance(p);
            Stmt *s = stmt_new(STMT_POINT_DIR, line); s->a = parse_paren_expr(p); return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "towards") == 0) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *tgt = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_POINT_TOWARDS, line); s->target = tgt; return s;
        }
        error(p, line, "unknown 'point' statement");
        return NULL;
    }

    /* ── looks ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "say") == 0) {
        advance(p);
        Expr *msg = parse_paren_expr(p);
        if (check(p, TOK_FOR)) {
            advance(p);
            Expr *secs = parse_paren_expr(p);
            if (check(p, TOK_SECONDS) || (check(p, TOK_IDENT) && strcmp(cur(p).value, "second") == 0)) advance(p);
            Stmt *s = stmt_new(STMT_SAY_SECS, line); s->a = msg; s->secs = secs; return s;
        }
        Stmt *s = stmt_new(STMT_SAY, line); s->a = msg; return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "think") == 0) {
        advance(p);
        Expr *msg = parse_paren_expr(p);
        if (check(p, TOK_FOR)) {
            advance(p);
            Expr *secs = parse_paren_expr(p);
            if (check(p, TOK_SECONDS) || (check(p, TOK_IDENT) && strcmp(cur(p).value, "second") == 0)) advance(p);
            Stmt *s = stmt_new(STMT_THINK_SECS, line); s->a = msg; s->secs = secs; return s;
        }
        Stmt *s = stmt_new(STMT_THINK, line); s->a = msg; return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "switch") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "costume") == 0) {
            advance(p); expect(p, TOK_TO);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_SWITCH_COSTUME, line); s->name = name; return s;
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "backdrop") == 0) {
            advance(p); expect(p, TOK_TO);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_SWITCH_BACKDROP, line); s->name = name; return s;
        }
        error(p, line, "expected 'costume' or 'backdrop' after 'switch'");
        return NULL;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "next") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "costume") == 0) {
            advance(p);
            return stmt_new(STMT_NEXT_COSTUME, line);
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "backdrop") == 0) {
            advance(p);
            return stmt_new(STMT_NEXT_BACKDROP, line);
        }
        error(p, line, "expected 'costume' or 'backdrop' after 'next'");
        return NULL;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "show") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "variable") == 0) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_SHOW_VAR, line); s->name = name; return s;
        }
        if (check(p, TOK_LIST)) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_SHOW_LIST, line); s->name = strdup(name); return s;
        }
        return stmt_new(STMT_SHOW, line);
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "hide") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "variable") == 0) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_HIDE_VAR, line); s->name = name; return s;
        }
        if (check(p, TOK_LIST)) {
            advance(p);
            expect(p, TOK_LPAREN);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_HIDE_LIST, line); s->name = strdup(name); return s;
        }
        return stmt_new(STMT_HIDE, line);
    }

    /* clear graphic effects */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "clear") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "graphic") == 0) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "effects") == 0) {
                advance(p);
                return stmt_new(STMT_CLEAR_EFFECTS, line);
            }
        }
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "sound") == 0) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "effects") == 0) {
                advance(p);
                return stmt_new(STMT_CLEAR_SOUND_EFFECTS, line);
            }
        }
        error(p, line, "expected 'graphic effects' or 'sound effects' after 'clear'");
        return NULL;
    }

    /* ── sound ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "play") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "sound") == 0) advance(p);
        expect(p, TOK_LPAREN);
        char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "until") == 0) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "done") == 0) advance(p);
            Stmt *s = stmt_new(STMT_PLAY_SOUND_UNTIL, line); s->name = name; return s;
        }
        Stmt *s = stmt_new(STMT_PLAY_SOUND, line); s->name = name; return s;
    }

    /* ── events ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "broadcast") == 0) {
        advance(p);
        /* new: broadcast message (name) */
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "message") == 0) advance(p);
        expect(p, TOK_LPAREN);
        char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        if (check(p, TOK_AND)) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "wait") == 0) advance(p);
            Stmt *s = stmt_new(STMT_BROADCAST_WAIT, line); s->name = name; return s;
        }
        Stmt *s = stmt_new(STMT_BROADCAST, line); s->name = name; return s;
    }

    /* ── sensing ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "ask") == 0) {
        advance(p);
        Stmt *s = stmt_new(STMT_ASK, line); s->a = parse_paren_expr(p); return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "reset") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "timer") == 0) advance(p);
        return stmt_new(STMT_RESET_TIMER, line);
    }

    /* ── lists ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "add") == 0) {
        advance(p);
        Expr *item;
        if (check(p, TOK_LPAREN))
            item = parse_paren_expr(p);   /* (expr) — parse_paren_expr handles parens */
        else
            item = parse_expr(p);          /* bare expr like: mouse y, current year, variable (x) */
        expect(p, TOK_TO);
        /* new: "list (name)" or legacy "<name>" */
        if (check(p, TOK_LIST)) advance(p);
        if (check(p, TOK_LPAREN)) {
            advance(p);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            Stmt *s = stmt_new(STMT_LIST_ADD, line);
            s->name = strdup(name); s->a = item; return s;
        }
        expect(p, TOK_LANGLE);
        char *name = read_name_until(p, TOK_RANGLE, TOK_EOF);
        expect(p, TOK_RANGLE);
        Stmt *s = stmt_new(STMT_LIST_ADD, line);
        s->name = strdup(name); s->a = item; return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "delete") == 0) {
        Token nx = peek(p);
        if (nx.type == TOK_IDENT && strcmp(nx.value, "this") == 0) {
            advance(p); advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "clone") == 0) advance(p);
            return stmt_new(STMT_DELETE_CLONE, line);
        }
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "delete") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "all") == 0) {
            advance(p);
            if (check(p, TOK_OF)) advance(p);
            if (check(p, TOK_LIST)) advance(p);
            if (check(p, TOK_LPAREN)) {
                advance(p);
                char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
                Stmt *s = stmt_new(STMT_LIST_DELETE_ALL, line); s->name = strdup(name); return s;
            }
            expect(p, TOK_LANGLE);
            char *name = read_name_until(p, TOK_RANGLE, TOK_EOF); expect(p, TOK_RANGLE);
            Stmt *s = stmt_new(STMT_LIST_DELETE_ALL, line); s->name = strdup(name); return s;
        }
        Expr *idx = parse_paren_expr(p);
        expect(p, TOK_OF);
        if (check(p, TOK_LIST)) advance(p);
        if (check(p, TOK_LPAREN)) {
            advance(p);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
            /* Bug 6: consume optional second "of list (name)" suffix */
            if (check(p, TOK_OF)) { advance(p); if (check(p, TOK_LIST)) advance(p);
                if (check(p, TOK_LPAREN)) { advance(p); free(read_name_until(p, TOK_RPAREN, TOK_EOF)); expect(p, TOK_RPAREN); } }
            Stmt *s = stmt_new(STMT_LIST_DELETE, line); s->name = strdup(name); s->a = idx; return s;
        }
        expect(p, TOK_LANGLE);
        char *name = read_name_until(p, TOK_RANGLE, TOK_EOF); expect(p, TOK_RANGLE);
        Stmt *s = stmt_new(STMT_LIST_DELETE, line); s->name = strdup(name); s->a = idx; return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "insert") == 0) {
        advance(p);
        Expr *item = parse_paren_expr(p);
        expect(p, TOK_AT);
        Expr *idx = parse_paren_expr(p);
        expect(p, TOK_OF);
        if (check(p, TOK_LIST)) advance(p);
        if (check(p, TOK_LPAREN)) {
            advance(p);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
            /* Bug 6: consume optional second "of list (name)" suffix */
            if (check(p, TOK_OF)) { advance(p); if (check(p, TOK_LIST)) advance(p);
                if (check(p, TOK_LPAREN)) { advance(p); free(read_name_until(p, TOK_RPAREN, TOK_EOF)); expect(p, TOK_RPAREN); } }
            Stmt *s = stmt_new(STMT_LIST_INSERT, line);
            s->name = strdup(name); s->a = item; s->b = idx; return s;
        }
        expect(p, TOK_LANGLE);
        char *name = read_name_until(p, TOK_RANGLE, TOK_EOF); expect(p, TOK_RANGLE);
        Stmt *s = stmt_new(STMT_LIST_INSERT, line);
        s->name = strdup(name); s->a = item; s->b = idx; return s;
    }
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "replace") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "item") == 0) advance(p);
        Expr *idx = parse_paren_expr(p);
        expect(p, TOK_OF);
        if (check(p, TOK_LIST)) advance(p);
        if (check(p, TOK_LPAREN)) {
            advance(p);
            char *name = read_name_until(p, TOK_RPAREN, TOK_EOF); expect(p, TOK_RPAREN);
            /* Bug 6: consume optional second "of list (name)" suffix */
            if (check(p, TOK_OF)) { advance(p); if (check(p, TOK_LIST)) advance(p);
                if (check(p, TOK_LPAREN)) { advance(p); free(read_name_until(p, TOK_RPAREN, TOK_EOF)); expect(p, TOK_RPAREN); } }
            expect(p, TOK_WITH);
            Expr *val = parse_paren_expr(p);
            Stmt *s = stmt_new(STMT_LIST_REPLACE, line);
            s->name = strdup(name); s->a = idx; s->b = val; return s;
        }
        expect(p, TOK_LANGLE);
        char *name = read_name_until(p, TOK_RANGLE, TOK_EOF); expect(p, TOK_RANGLE);
        expect(p, TOK_WITH);
        Expr *val = parse_paren_expr(p);
        Stmt *s = stmt_new(STMT_LIST_REPLACE, line);
        s->name = strdup(name); s->a = idx; s->b = val; return s;
    }

    /* ── create clone of (target) ── */
    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "create") == 0) {
        advance(p);
        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "clone") == 0) advance(p);
        if (check(p, TOK_OF)) advance(p);
        expect(p, TOK_LPAREN);
        char *tgt = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);
        Stmt *s = stmt_new(STMT_CREATE_CLONE, line);
        s->target = (strcmp(tgt, "myself") == 0) ? (free(tgt), strdup("_myself_")) : tgt;
        return s;
    }

    /* proc call */
    if (check(p, TOK_IDENT)) {
        int is_proc = 0;
        int proc_idx = -1;
        int best_words = 0;

        for (int i = 0; i < p->proc_count; i++) {
            char *tmp = strdup(p->procs[i].name);
            char *words[64]; int wc = 0;
            char *w = strtok(tmp, " ");
            while (w && wc < 63) { words[wc++] = w; w = strtok(NULL, " "); }

            if (wc == 0) { free(tmp); continue; }

            if (!cur(p).value || strcmp(cur(p).value, words[0]) != 0) { free(tmp); continue; }

            if (wc == 1) {
                if (1 > best_words) { is_proc = 1; proc_idx = i; best_words = 1; }
                free(tmp); continue;
            }

            int saved_pos  = p->lexer.pos;
            int saved_line = p->lexer.line;
            int had_peeked = p->has_peeked;
            Token peeked_save = p->peeked;

            int matched = 1;
            Token probed[63];
            int probed_count = 0;

            if (had_peeked) {
                if (!peeked_save.value || strcmp(peeked_save.value, words[1]) != 0)
                    matched = 0;
                else {
                    for (int wi = 2; wi < wc && matched; wi++) {
                        Token t = lexer_next(&p->lexer);
                        probed[probed_count++] = t;
                        if (!t.value || strcmp(t.value, words[wi]) != 0) matched = 0;
                    }
                }
            } else {
                for (int wi = 1; wi < wc && matched; wi++) {
                    Token t = lexer_next(&p->lexer);
                    probed[probed_count++] = t;
                    if (!t.value || strcmp(t.value, words[wi]) != 0) matched = 0;
                }
            }

            p->lexer.pos  = saved_pos;
            p->lexer.line = saved_line;
            for (int k = 0; k < probed_count; k++) token_free(&probed[k]);

            if (matched && wc > best_words) {
                is_proc = 1; proc_idx = i; best_words = wc;
            }
            free(tmp);
        }

        if (is_proc) {
            char *pname = strdup(p->procs[proc_idx].name);
            advance(p);
            for (int wi = 1; wi < best_words; wi++) advance(p);
            Stmt *s = stmt_new(STMT_PROC_CALL, line);
            s->name = pname;
            int nparams = (proc_idx >= 0) ? p->procs[proc_idx].param_count : 0;
            s->args = NULL; s->arg_count = 0;
            for (int i = 0; i < nparams; i++) {
                if (!check(p, TOK_LPAREN) && !is_structured_expr_start(p)) break;
                Expr *arg = parse_paren_expr(p);
                s->args = realloc(s->args, (s->arg_count+1)*sizeof(Expr*));
                s->args[s->arg_count++] = arg;
            }
            return s;
        }
    }

    /* fallback: treat unknown ident as a zero-arg proc call
       (handles calls to procs defined in other sprites or missing custom block decls) */
    if (check(p, TOK_IDENT)) {
        char *pname = strdup(cur(p).value);
        advance(p);
        /* collect any paren-arg tokens */
        Stmt *s = stmt_new(STMT_PROC_CALL, line);
        s->name = pname;
        s->args = NULL; s->arg_count = 0;
        while (check(p, TOK_LPAREN)) {
            Expr *arg = parse_paren_expr(p);
            s->args = realloc(s->args, (s->arg_count+1)*sizeof(Expr*));
            s->args[s->arg_count++] = arg;
        }
        return s;
    }

    errorf(p, line, "unknown statement '%s'",
           cur(p).value ? cur(p).value : token_type_name(cur(p).type));
    advance(p);
    return NULL;
}

/* ── top-level parser ────────────────────────────────────────────── */

static VarDecl *parse_var_decl(Parser *p) {
    int line = cur(p).line;
    expect(p, TOK_VAR);
    expect(p, TOK_LPAREN);
    char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
    expect(p, TOK_RPAREN);
    VarDecl *d = calloc(1, sizeof(VarDecl));
    d->name = name;
    d->line = line;
    if (check(p, TOK_EQUAL)) {
        advance(p);
        d->init = parse_paren_expr(p);
    }
    if (check(p, TOK_FLAG_GLOBAL)) { advance(p); d->is_global = 1; }
    return d;
}

static ListDecl *parse_list_decl(Parser *p) {
    int line = cur(p).line;
    expect(p, TOK_LIST);
    expect(p, TOK_LPAREN);
    char *name = read_name_until(p, TOK_RPAREN, TOK_EOF);
    expect(p, TOK_RPAREN);
    ListDecl *d = calloc(1, sizeof(ListDecl));
    d->name = strdup(name);
    d->line = line;
    if (check(p, TOK_FLAG_GLOBAL)) { advance(p); d->is_global = 1; }
    return d;
}

static Script *parse_script(Parser *p) {
    int line = cur(p).line;

    if (check(p, TOK_LBRACE)) {
        Script *sc = script_new(HAT_NONE, NULL, line);
        parse_body(p, &sc->body, &sc->body_count);
        return sc;
    }

    if (check(p, TOK_IDENT) && strcmp(cur(p).value, "define") == 0
        && peek(p).type == TOK_LPAREN) {
        advance(p);
        expect(p, TOK_LPAREN);
        char *pname = read_name_until(p, TOK_RPAREN, TOK_EOF);
        expect(p, TOK_RPAREN);

        Script *sc = script_new(HAT_PROCEDURE_DEF, pname, line);
        free(pname);

        int no_refresh = 0;
        char **params = NULL; int nparams = 0;
        for (int i = 0; i < p->proc_count; i++) {
            if (strcmp(p->procs[i].name, sc->hat_arg) == 0) {
                params   = p->procs[i].params;
                nparams  = p->procs[i].param_count;
                break;
            }
        }
        sc->proc_params       = params;
        sc->proc_param_count  = nparams;
        sc->no_refresh        = no_refresh;

        /* skip parameter slot tokens: define (name) (param1) (param2) ... { */
        while (check(p, TOK_LPAREN)) {
            advance(p);
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) advance(p);
            if (check(p, TOK_RPAREN)) advance(p);
        }

        char **saved_params    = p->cur_params;
        int    saved_nparams   = p->cur_param_count;
        p->cur_params          = params;
        p->cur_param_count     = nparams;

        parse_body(p, &sc->body, &sc->body_count);

        p->cur_params      = saved_params;
        p->cur_param_count = saved_nparams;
        return sc;
    }

    if (check(p, TOK_WHEN)) {
        advance(p);
        expect(p, TOK_LPAREN);
        HatType hat = HAT_NONE;
        char *hat_arg = NULL;

        if (check(p, TOK_IDENT) && strcmp(cur(p).value, "green") == 0) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "flag") == 0) advance(p);
            hat = HAT_GREEN_FLAG;
        } else if (check(p, TOK_IDENT) && strcmp(cur(p).value, "message") == 0) {
            /* legacy: when (message name) */
            advance(p);
            hat_arg = read_name_until(p, TOK_RPAREN, TOK_EOF);
            hat = HAT_MESSAGE;
        } else if (check(p, TOK_IDENT) && strcmp(cur(p).value, "key") == 0) {
            advance(p);
            hat_arg = read_name_until(p, TOK_RPAREN, TOK_EOF);
            char *pressed = strstr(hat_arg, " pressed");
            if (pressed) *pressed = '\0';
            hat = HAT_KEY_PRESSED;
        } else if (check(p, TOK_IDENT) && strcmp(cur(p).value, "this") == 0) {
            advance(p);
            while (check(p, TOK_IDENT) || check(p, TOK_SPRITE)) advance(p);
            hat = HAT_SPRITE_CLICKED;
        } else if (check(p, TOK_IDENT) && strcmp(cur(p).value, "start") == 0) {
            Token nx = peek(p);
            if (nx.type == TOK_IDENT && strcmp(nx.value, "as") == 0) {
                advance(p);
                while (check(p, TOK_IDENT)) advance(p);
                hat = HAT_CLONE_START;
            } else {
                hat_arg = read_name_until(p, TOK_RPAREN, TOK_EOF);
                hat = HAT_MESSAGE;
            }
        } else if (check(p, TOK_IDENT) && strcmp(cur(p).value, "backdrop") == 0) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "switches") == 0) advance(p);
            if (check(p, TOK_TO) || (check(p, TOK_IDENT) && strcmp(cur(p).value, "to") == 0)) advance(p);
            hat_arg = read_name_until(p, TOK_RPAREN, TOK_EOF);
            hat = HAT_BACKDROP_SWITCHES;
        } else if (check(p, TOK_IDENT) &&
                   (strcmp(cur(p).value, "loudness") == 0 ||
                    strcmp(cur(p).value, "timer") == 0)) {
            int is_loudness = strcmp(cur(p).value, "loudness") == 0;
            advance(p);
            hat_arg = strdup(is_loudness ? "LOUDNESS" : "TIMER");
            if (check(p, TOK_RANGLE)) advance(p);
            expect(p, TOK_RPAREN);
            Script *sc2 = script_new(HAT_GREATER_THAN, hat_arg, line);
            free(hat_arg);
            sc2->hat_threshold = parse_paren_expr(p);
            parse_body(p, &sc2->body, &sc2->body_count);
            return sc2;
        } else {
            /* new syntax: when (messageName) — bare name is the message */
            hat_arg = read_name_until(p, TOK_RPAREN, TOK_EOF);
            hat = HAT_MESSAGE;
        }
        expect(p, TOK_RPAREN);

        Script *sc = script_new(hat, hat_arg, line);
        free(hat_arg);
        parse_body(p, &sc->body, &sc->body_count);
        return sc;
    }

    Script *sc = script_new(HAT_NONE, NULL, line);
    Stmt *s = parse_stmt(p);
    if (s) {
        sc->body = malloc(sizeof(Stmt*));
        sc->body[0] = s;
        sc->body_count = 1;
    }
    return sc;
}

static Sprite *parse_sprite(Parser *p) {
    int line = cur(p).line;
    int is_stage = 0;

    if (check(p, TOK_STAGE)) {
        is_stage = 1;
        advance(p);
    } else {
        expect(p, TOK_SPRITE);
    }

    char name_buf[256] = {0};
    if (check(p, TOK_LPAREN)) {
        advance(p);
        char *n = read_name_until(p, TOK_RPAREN, TOK_EOF);
        strncpy(name_buf, n, sizeof(name_buf)-1);
        free(n);
        expect(p, TOK_RPAREN);
    } else {
        /* fallback: read until '{' for compatibility */
        int first = 1;
        while (!check(p, TOK_LBRACE) && !check(p, TOK_EOF)) {
            if (!first) strncat(name_buf, " ", sizeof(name_buf)-strlen(name_buf)-1);
            strncat(name_buf, cur(p).value ? cur(p).value : "", sizeof(name_buf)-strlen(name_buf)-1);
            first = 0;
            advance(p);
        }
        int len = strlen(name_buf);
        while (len > 0 && isspace((unsigned char)name_buf[len-1])) name_buf[--len] = '\0';
    }

    Sprite *sp = sprite_new(is_stage ? NULL : name_buf, is_stage);
    expect(p, TOK_LBRACE);
    for (int i = 0; i < p->proc_count; i++) free(p->procs[i].name);
    p->proc_count = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (check(p, TOK_VAR)) {
            VarDecl *d = parse_var_decl(p);
            sp->vars = realloc(sp->vars, (sp->var_count+1)*sizeof(VarDecl*));
            sp->vars[sp->var_count++] = d;
        } else if (check(p, TOK_LIST)) {
            ListDecl *d = parse_list_decl(p);
            sp->lists = realloc(sp->lists, (sp->list_count+1)*sizeof(ListDecl*));
            sp->lists[sp->list_count++] = d;
        } else if (check(p, TOK_IDENT) && strcmp(cur(p).value, "custom") == 0) {
            advance(p);
            if (check(p, TOK_IDENT) && strcmp(cur(p).value, "block") == 0) advance(p);
            int decl_line = cur(p).line;
            expect(p, TOK_LPAREN);
            char *bname = read_name_until(p, TOK_RPAREN, TOK_EOF);
            expect(p, TOK_RPAREN);
            char **params = NULL; int nparams = 0;
            while (check(p, TOK_LPAREN)) {
                advance(p);
                char *pname = read_name_until(p, TOK_RPAREN, TOK_EOF);
                expect(p, TOK_RPAREN);
                params = realloc(params, (nparams+1)*sizeof(char*));
                params[nparams++] = strdup(pname);
            }
            int no_refresh = 0;
            if (check(p, TOK_UNKNOWN) && cur(p).value && strstr(cur(p).value,"no-refresh")) {
                no_refresh=1; advance(p);
            }
            if (p->proc_count >= p->proc_cap) {
                p->proc_cap = p->proc_cap ? p->proc_cap*2 : 16;
                p->procs = realloc(p->procs, p->proc_cap*sizeof(ProcEntry));
            }
            p->procs[p->proc_count].name        = strdup(bname);
            p->procs[p->proc_count].params      = params;
            p->procs[p->proc_count].param_count = nparams;
            p->proc_count++;
            ProcDecl *pd = calloc(1, sizeof(ProcDecl));
            pd->name = strdup(bname); pd->no_refresh = no_refresh; pd->line = decl_line;
            pd->params = calloc(nparams, sizeof(ProcParam*)); pd->param_count = nparams;
            for (int i = 0; i < nparams; i++) {
                pd->params[i] = calloc(1, sizeof(ProcParam));
                pd->params[i]->name = strdup(params[i]);
            }
            sp->procs = realloc(sp->procs, (sp->proc_count+1)*sizeof(ProcDecl*));
            sp->procs[sp->proc_count++] = pd;
        } else {
            Script *sc = parse_script(p);
            if (sc) {
                sp->scripts = realloc(sp->scripts, (sp->script_count+1)*sizeof(Script*));
                sp->scripts[sp->script_count++] = sc;
            }
        }
    }

    if (check(p, TOK_EOF))
        errorf(p, line, "unclosed sprite block '%s' starting at line %d",
               sp->name ? sp->name : "stage", line);
    else
        expect(p, TOK_RBRACE);

    return sp;
}

void parser_init(Parser *p, const char *src) {
    memset(p, 0, sizeof(Parser));
    lexer_init(&p->lexer, src);
    advance(p);
}

Program *parser_parse(Parser *p) {
    Program *prog = program_new();

    while (!check(p, TOK_EOF)) {
        if (check(p, TOK_VAR)) {
            VarDecl *d = parse_var_decl(p);
            prog->globals = realloc(prog->globals, (prog->global_count+1)*sizeof(VarDecl*));
            prog->globals[prog->global_count++] = d;
        } else if (check(p, TOK_LIST)) {
            ListDecl *d = parse_list_decl(p);
            prog->global_lists = realloc(prog->global_lists, (prog->global_list_count+1)*sizeof(ListDecl*));
            prog->global_lists[prog->global_list_count++] = d;
        } else if (check(p, TOK_SPRITE) || check(p, TOK_STAGE)) {
            Sprite *sp = parse_sprite(p);
            prog->sprites = realloc(prog->sprites, (prog->sprite_count+1)*sizeof(Sprite*));
            prog->sprites[prog->sprite_count++] = sp;
        } else {
            errorf(p, cur(p).line, "unexpected '%s' at top level",
                   cur(p).value ? cur(p).value : token_type_name(cur(p).type));
            advance(p);
        }
    }

    return prog;
}
