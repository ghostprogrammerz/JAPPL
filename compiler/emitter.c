#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "emitter.h"
#include "ast.h"

/* ── simple growable string buffer ─────────────────────────────── */
typedef struct { char *buf; int len; int cap; } Buf;

const char *state_get_var(const StateTable *st, const char *name) {
    if (!st) return NULL;
    for (int i = 0; i < st->var_count; i++)
        if (strcmp(st->vars[i].name, name) == 0) return st->vars[i].value;
    return NULL;
}

const StateList *state_get_list(const StateTable *st, const char *name) {
    if (!st) return NULL;
    for (int i = 0; i < st->list_count; i++)
        if (strcmp(st->lists[i].name, name) == 0) return &st->lists[i];
    return NULL;
}

void state_table_free(StateTable *st) {
    if (!st) return;
    for (int i = 0; i < st->var_count; i++) { free(st->vars[i].name); free(st->vars[i].value); }
    free(st->vars);
    for (int i = 0; i < st->list_count; i++) {
        free(st->lists[i].name);
        for (int j = 0; j < st->lists[i].count; j++) free(st->lists[i].items[j]);
        free(st->lists[i].items);
    }
    free(st->lists);
}

static void buf_init(Buf *b) {
    b->buf = malloc(4096);
    b->cap = 4096;
    b->len = 0;
    b->buf[0]=0;
}

static void buf_ensure(Buf *b, int n) {
    while (b->len + n + 1 >= b->cap) {
        b->cap *= 2;
        b->buf = realloc(b->buf, b->cap);
    }
}

static void buf_cat(Buf *b, const char *s) {
    int n = strlen(s);
    buf_ensure(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buf_cat(b, tmp);
}

/* ── UUID generation ────────────────────────────────────────────── */
static unsigned long long uid_counter = 1000000;
static void new_uid(char *out) {
    snprintf(out, 32, "%020llu", uid_counter++);
}

/* ── JSON string escaping ───────────────────────────────────────── */
/* emit a value as a JSON number if it looks numeric, else as a JSON string */
static void buf_json_str(Buf *b, const char *s) {
    buf_cat(b, "\"");
    if (!s) { buf_cat(b, "\""); return; }
    for (; *s; s++) {
        switch (*s) {
            case '"':  buf_cat(b, "\\\""); break;
            case '\\': buf_cat(b, "\\\\"); break;
            case '\n': buf_cat(b, "\\n");  break;
            case '\r': buf_cat(b, "\\r");  break;
            case '\t': buf_cat(b, "\\t");  break;
            default: { char c[2]={*s,0}; buf_cat(b, c); }
        }
    }
    buf_cat(b, "\"");
}

static void buf_json_val(Buf *b, const char *s) {
    if (!s || !*s) { buf_cat(b, "\"\""); return; }
    char *end;
    strtod(s, &end);
    if (end != s && *end == '\0') {
        buf_cat(b, s);
    } else {
        buf_json_str(b, s);
    }
}

/* ── Block table ────────────────────────────────────────────────── */
typedef struct { char uid[32]; char *json; } BlockEntry;
typedef struct { BlockEntry *entries; int count; int cap; } BlockTable;

static void btable_init(BlockTable *t) {
    t->entries = malloc(256*sizeof(BlockEntry));
    t->cap=256;
    t->count=0;
}

static void btable_add(BlockTable *t, const char *uid, const char *json) {
    if (t->count >= t->cap) {
        t->cap*=2;
        t->entries=realloc(t->entries, t->cap*sizeof(BlockEntry));
    }
    strncpy(t->entries[t->count].uid, uid, 31);
    t->entries[t->count].uid[31] = 0;
    t->entries[t->count].json = strdup(json);
    t->count++;
}

/* ── emit a full block object ───────────────────────────────────── */
static void emit_block(BlockTable *bt, const char *uid, const char *opcode,
                        const char *parent, const char *next,
                        const char *inputs, const char *fields,
                        int topLevel, int shadow, double x, double y) {
    Buf b;
    buf_init(&b);

    buf_printf(&b, "{\"opcode\":");
    buf_json_str(&b, opcode);

    if (next && next[0])
        buf_printf(&b, ",\"next\":\"%s\"", next);
    else
        buf_printf(&b, ",\"next\":null");

    if (parent && parent[0])
        buf_printf(&b, ",\"parent\":\"%s\"", parent);
    else
        buf_printf(&b, ",\"parent\":null");

    buf_printf(&b, ",\"inputs\":{%s},\"fields\":{%s}",
               inputs && inputs[0] ? inputs : "",
               fields && fields[0] ? fields : "");

    buf_printf(&b, ",\"shadow\":%s,\"topLevel\":%s",
               shadow ? "true" : "false",
               topLevel ? "true" : "false");

    if (topLevel)
        buf_printf(&b, ",\"x\":%.0f,\"y\":%.0f", x, y);

    buf_cat(&b, "}");
    btable_add(bt, uid, b.buf);
    free(b.buf);
}

/* ── forward declarations ───────────────────────────────────────── */
/* Emit a boolean CONDITION input.  When cond is a bare number/string (meaning
   the original slot was empty and the decompiler emitted "0" as a placeholder),
   skip emitting the slot so Scratch sees an empty boolean. */
#define EMIT_CONDITION(bt_, cond_, uid_, inputs_) do { \
    if ((cond_) && !((cond_)->type == EXPR_NUMBER)) { \
        char _sub[32] = {0}; \
        emit_reporter((bt_), (cond_), (uid_), _sub); \
        buf_printf(&(inputs_), "\"CONDITION\":[2,\"%s\"]", _sub); \
    } \
} while(0)

static void emit_reporter(BlockTable *bt, Expr *e, const char *parent_uid, char *out_uid);
static void input_expr(BlockTable *bt, Expr *e, const char *name,
                        const char *parent, Buf *out);

/* ── expression → Scratch input JSON fragment ───────────────────── */
static void expr_to_input(BlockTable *bt, Expr *e, const char *input_name,
                           const char *parent_uid, Buf *out) {
    if (!e) {
        buf_printf(out, "\"%s\":[1,[10,\"\"]]", input_name);
        return;
    }
    switch (e->type) {
        case EXPR_NUMBER: {
            buf_printf(out, "\"%s\":[1,[4,\"%g\"]]", input_name, e->number);
            break;
        }
        case EXPR_STRING: {
            Buf tmp; buf_init(&tmp);
            buf_printf(&tmp, "\"%s\":[1,[10,", input_name);
            buf_json_str(&tmp, e->str);
            buf_cat(&tmp, "]]");
            buf_cat(out, tmp.buf);
            free(tmp.buf);
            break;
        }
        case EXPR_VAR: {
            Buf tmp; buf_init(&tmp);
            buf_printf(&tmp, "\"%s\":[3,[12,", input_name);
            buf_json_str(&tmp, e->str);
            buf_cat(&tmp, ",");
            buf_json_str(&tmp, e->str);
            buf_cat(&tmp, "],[10,\"\"]]");
            buf_cat(out, tmp.buf);
            free(tmp.buf);
            break;
        }
        case EXPR_ARG_REPORTER: {
            char uid[32]; new_uid(uid);
            Buf fields; buf_init(&fields);
            buf_printf(&fields, "\"VALUE\":[\"%s\",null]", e->str ? e->str : "");
            emit_block(bt, uid, "argument_reporter_string_number", parent_uid, "", "", fields.buf, 0, 0, 0, 0);
            free(fields.buf);
            buf_printf(out, "\"%s\":[3,\"%s\",[10,\"\"]]", input_name, uid);
            break;
        }
        case EXPR_ARG_REPORTER_BOOL: {
            char uid[32]; new_uid(uid);
            Buf fields; buf_init(&fields);
            buf_printf(&fields, "\"VALUE\":[\"%s\",null]", e->str ? e->str : "");
            emit_block(bt, uid, "argument_reporter_boolean", parent_uid, "", "", fields.buf, 0, 0, 0, 0);
            free(fields.buf);
            buf_printf(out, "\"%s\":[2,\"%s\"]", input_name, uid);
            break;
        }
        default: {
            char uid[32];
            emit_reporter(bt, e, parent_uid, uid);
            buf_printf(out, "\"%s\":[3,\"%s\",[10,\"\"]]", input_name, uid);
            break;
        }
    }
}

/* ── emit a reporter block ──────────────────────────────────────── */
static void emit_reporter(BlockTable *bt, Expr *e, const char *parent_uid, char *out_uid) {
    new_uid(out_uid);
    Buf inputs; buf_init(&inputs);
    Buf fields; buf_init(&fields);

    switch (e->type) {
        case EXPR_BINOP: {
            const char *opcode =
                strcmp(e->binop.op,"+")==0   ? "operator_add" :
                strcmp(e->binop.op,"-")==0   ? "operator_subtract" :
                strcmp(e->binop.op,"*")==0   ? "operator_multiply" :
                strcmp(e->binop.op,"/")==0   ? "operator_divide" :
                strcmp(e->binop.op,"mod")==0 ? "operator_mod" :
                strcmp(e->binop.op,"=")==0   ? "operator_equals" :
                strcmp(e->binop.op,">")==0   ? "operator_gt" :
                strcmp(e->binop.op,"<")==0   ? "operator_lt" :
                strcmp(e->binop.op,"and")==0 ? "operator_and" :
                strcmp(e->binop.op,"or")==0  ? "operator_or" : "operator_add";
            int is_cmp = (strcmp(e->binop.op,"=")==0 || strcmp(e->binop.op,">")==0 ||
                          strcmp(e->binop.op,"<")==0 || strcmp(e->binop.op,"and")==0 ||
                          strcmp(e->binop.op,"or")==0);
            const char *lname = is_cmp ? "OPERAND1" : "NUM1";
            const char *rname = is_cmp ? "OPERAND2" : "NUM2";
            char luid[32] = {0}, ruid[32] = {0};
            int llit = (e->binop.left->type==EXPR_NUMBER||e->binop.left->type==EXPR_STRING||e->binop.left->type==EXPR_VAR||e->binop.left->type==EXPR_ARG_REPORTER||e->binop.left->type==EXPR_ARG_REPORTER_BOOL);
            int rlit = (e->binop.right->type==EXPR_NUMBER||e->binop.right->type==EXPR_STRING||e->binop.right->type==EXPR_VAR||e->binop.right->type==EXPR_ARG_REPORTER||e->binop.right->type==EXPR_ARG_REPORTER_BOOL);
            if (!llit) emit_reporter(bt, e->binop.left,  out_uid, luid);
            if (!rlit) emit_reporter(bt, e->binop.right, out_uid, ruid);
            Buf li; buf_init(&li);
            if (llit) expr_to_input(bt, e->binop.left, lname, out_uid, &li);
            else buf_printf(&li, "\"%s\":[3,\"%s\",[10,\"\"]]", lname, luid);
            buf_cat(&inputs, li.buf); free(li.buf);
            buf_cat(&inputs, ",");
            Buf ri; buf_init(&ri);
            if (rlit) expr_to_input(bt, e->binop.right, rname, out_uid, &ri);
            else buf_printf(&ri, "\"%s\":[3,\"%s\",[10,\"\"]]", rname, ruid);
            buf_cat(&inputs, ri.buf); free(ri.buf);
            emit_block(bt, out_uid, opcode, parent_uid, "", inputs.buf, "", 0, 0, 0, 0);
            break;
        }
        case EXPR_NOT: {
            char sub[32] = {0};
            int lit = (e->unary.expr->type==EXPR_NUMBER||e->unary.expr->type==EXPR_STRING||e->unary.expr->type==EXPR_VAR||e->unary.expr->type==EXPR_ARG_REPORTER||e->unary.expr->type==EXPR_ARG_REPORTER_BOOL);
            if (!lit) emit_reporter(bt, e->unary.expr, out_uid, sub);
            inputs.len=0; inputs.buf[0]=0;
            if (lit) { Buf tmp; buf_init(&tmp); expr_to_input(bt,e->unary.expr,"OPERAND",out_uid,&tmp); buf_cat(&inputs,tmp.buf); free(tmp.buf); }
            else buf_printf(&inputs, "\"OPERAND\":[3,\"%s\",[10,\"\"]]", sub);
            emit_block(bt, out_uid, "operator_not", parent_uid, "", inputs.buf, "", 0, 0, 0, 0);
            break;
        }
        case EXPR_PICK_RANDOM: {
            Buf f; buf_init(&f); Buf t2; buf_init(&t2);
            expr_to_input(bt, e->pair.a, "FROM", out_uid, &f);
            expr_to_input(bt, e->pair.b, "TO",   out_uid, &t2);
            buf_printf(&inputs, "%s,%s", f.buf, t2.buf);
            free(f.buf); free(t2.buf);
            emit_block(bt, out_uid, "operator_random", parent_uid, "", inputs.buf, "", 0, 0, 0, 0);
            break;
        }
        case EXPR_JOIN: {
            Buf a; buf_init(&a); Buf b2; buf_init(&b2);
            expr_to_input(bt, e->pair.a, "STRING1", out_uid, &a);
            expr_to_input(bt, e->pair.b, "STRING2", out_uid, &b2);
            buf_printf(&inputs, "%s,%s", a.buf, b2.buf);
            free(a.buf); free(b2.buf);
            emit_block(bt, out_uid, "operator_join", parent_uid, "", inputs.buf, "", 0, 0, 0, 0);
            break;
        }
        case EXPR_LENGTH_OF: {
            Buf s; buf_init(&s);
            expr_to_input(bt, e->unary.expr, "STRING", out_uid, &s);
            buf_cat(&inputs, s.buf); free(s.buf);
            emit_block(bt, out_uid, "operator_length", parent_uid, "", inputs.buf, "", 0, 0, 0, 0);
            break;
        }
        case EXPR_LETTER_OF: {
            Buf n; buf_init(&n); Buf s; buf_init(&s);
            expr_to_input(bt, e->letter_of.n,   "LETTER", out_uid, &n);
            expr_to_input(bt, e->letter_of.str, "STRING", out_uid, &s);
            buf_printf(&inputs, "%s,%s", n.buf, s.buf);
            free(n.buf); free(s.buf);
            emit_block(bt, out_uid, "operator_letter_of", parent_uid, "", inputs.buf, "", 0, 0, 0, 0);
            break;
        }
        case EXPR_ROUND: {
            Buf n; buf_init(&n);
            expr_to_input(bt, e->unary.expr, "NUM", out_uid, &n);
            buf_cat(&inputs, n.buf); free(n.buf);
            emit_block(bt, out_uid, "operator_round", parent_uid, "", inputs.buf, "", 0, 0, 0, 0);
            break;
        }
        case EXPR_MATH_FN: {
            Buf n; buf_init(&n);
            expr_to_input(bt, e->mathfn.arg, "NUM", out_uid, &n);
            buf_cat(&inputs, n.buf); free(n.buf);
            const char *scratch_fn = e->mathfn.fn;
            if (strcmp(scratch_fn,"exp")==0)   scratch_fn = "e ^";
            else if (strcmp(scratch_fn,"exp10")==0) scratch_fn = "10 ^";
            else if (strcmp(scratch_fn,"ln")==0)    scratch_fn = "ln";
            else if (strcmp(scratch_fn,"log")==0)   scratch_fn = "log";
            buf_printf(&fields, "\"OPERATOR\":[\"%s\",null]", scratch_fn);
            emit_block(bt, out_uid, "operator_mathop", parent_uid, "", inputs.buf, fields.buf, 0, 0, 0, 0);
            break;
        }
        case EXPR_MOUSE_X:    emit_block(bt,out_uid,"sensing_mousex",parent_uid,"","","",0,0,0,0); break;
        case EXPR_MOUSE_Y:    emit_block(bt,out_uid,"sensing_mousey",parent_uid,"","","",0,0,0,0); break;
        case EXPR_X_POS:      emit_block(bt,out_uid,"motion_xposition",parent_uid,"","","",0,0,0,0); break;
        case EXPR_Y_POS:      emit_block(bt,out_uid,"motion_yposition",parent_uid,"","","",0,0,0,0); break;
        case EXPR_ANSWER:     emit_block(bt,out_uid,"sensing_answer",parent_uid,"","","",0,0,0,0); break;
        case EXPR_TIMER:      emit_block(bt,out_uid,"sensing_timer",parent_uid,"","","",0,0,0,0); break;
        case EXPR_MOUSE_DOWN: emit_block(bt,out_uid,"sensing_mousedown",parent_uid,"","","",0,0,0,0); break;
        case EXPR_KEY_PRESSED: {
            char muid[32]; new_uid(muid);
            char mf[128]; snprintf(mf,sizeof(mf),"\"KEY_OPTION\":[\"%s\",null]",e->str?e->str:"");
            emit_block(bt,muid,"sensing_keyoptions",out_uid,"","",mf,0,1,0,0);
            buf_printf(&inputs,"\"KEY_OPTION\":[1,\"%s\"]",muid);
            emit_block(bt,out_uid,"sensing_keypressed",parent_uid,"",inputs.buf,"",0,0,0,0);
            break;
        }
        case EXPR_TOUCHING: {
            const char *tgt = e->touching.target ? e->touching.target : "";
            const char *val = strcmp(tgt,"mouse pointer")==0 ? "_mouse_" :
                              strcmp(tgt,"edge")==0          ? "_edge_"  : tgt;
            char muid[32]; new_uid(muid);
            char mf[128]; snprintf(mf,sizeof(mf),"\"TOUCHINGOBJECTMENU\":[\"%s\",null]",val);
            emit_block(bt,muid,"sensing_touchingobjectmenu",out_uid,"","",mf,0,1,0,0);
            buf_printf(&inputs,"\"TOUCHINGOBJECTMENU\":[1,\"%s\"]",muid);
            emit_block(bt,out_uid,"sensing_touchingobject",parent_uid,"",inputs.buf,"",0,0,0,0);
            break;
        }
        case EXPR_DISTANCE_TO: {
            const char *tgt = e->touching.target ? e->touching.target : "";
            const char *val = strcmp(tgt,"mouse pointer")==0 ? "_mouse_" : tgt;
            char muid[32]; new_uid(muid);
            char mf[128]; snprintf(mf,sizeof(mf),"\"DISTANCETOMENU\":[\"%s\",null]",val);
            emit_block(bt,muid,"sensing_distancetomenu",out_uid,"","",mf,0,1,0,0);
            buf_printf(&inputs,"\"DISTANCETOMENU\":[1,\"%s\"]",muid);
            emit_block(bt,out_uid,"sensing_distanceto",parent_uid,"",inputs.buf,"",0,0,0,0);
            break;
        }
        case EXPR_VAR: {
            buf_printf(&fields, "\"VARIABLE\":[\"%s\",\"%s\"]", e->str, e->str);
            emit_block(bt,out_uid,"data_variable",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_ARG_REPORTER: {
            buf_printf(&fields, "\"VALUE\":[\"%s\",null]", e->str ? e->str : "");
            emit_block(bt,out_uid,"argument_reporter_string_number",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_ARG_REPORTER_BOOL: {
            buf_printf(&fields, "\"VALUE\":[\"%s\",null]", e->str ? e->str : "");
            emit_block(bt,out_uid,"argument_reporter_boolean",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_LIST_ITEM: {
            Buf idx; buf_init(&idx);
            expr_to_input(bt, e->list_item.index, "INDEX", out_uid, &idx);
            buf_cat(&inputs, idx.buf); free(idx.buf);
            buf_printf(&fields, "\"LIST\":[\"%s\",\"%s\"]", e->list_item.list, e->list_item.list);
            emit_block(bt,out_uid,"data_itemoflist",parent_uid,"",inputs.buf,fields.buf,0,0,0,0);
            break;
        }
        case EXPR_LIST_LENGTH: {
            buf_printf(&fields, "\"LIST\":[\"%s\",\"%s\"]", e->list_len.list, e->list_len.list);
            emit_block(bt,out_uid,"data_lengthoflist",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_LIST_CONTAINS: {
            Buf v; buf_init(&v);
            expr_to_input(bt, e->list_contains.val, "ITEM", out_uid, &v);
            buf_cat(&inputs, v.buf); free(v.buf);
            buf_printf(&fields, "\"LIST\":[\"%s\",\"%s\"]", e->list_contains.list, e->list_contains.list);
            emit_block(bt,out_uid,"data_listcontainsitem",parent_uid,"",inputs.buf,fields.buf,0,0,0,0);
            break;
        }
        /* ── motion reporters ── */
        case EXPR_DIRECTION:
            emit_block(bt,out_uid,"motion_direction",parent_uid,"","","",0,0,0,0); break;

        /* ── looks reporters ── */
        case EXPR_SIZE:
            emit_block(bt,out_uid,"looks_size",parent_uid,"","","",0,0,0,0); break;
        case EXPR_COSTUME_NUM: {
            buf_printf(&fields,"\"NUMBER_NAME\":[\"number\",null]");
            emit_block(bt,out_uid,"looks_costumenumbername",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_COSTUME_NAME: {
            buf_printf(&fields,"\"NUMBER_NAME\":[\"name\",null]");
            emit_block(bt,out_uid,"looks_costumenumbername",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_BACKDROP_NUM: {
            buf_printf(&fields,"\"NUMBER_NAME\":[\"number\",null]");
            emit_block(bt,out_uid,"looks_backdropnumbername",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_BACKDROP_NAME: {
            buf_printf(&fields,"\"NUMBER_NAME\":[\"name\",null]");
            emit_block(bt,out_uid,"looks_backdropnumbername",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }

        /* ── sound reporters ── */
        case EXPR_VOLUME:
            emit_block(bt,out_uid,"sound_volume",parent_uid,"","","",0,0,0,0); break;

        /* ── sensing reporters ── */
        case EXPR_LOUDNESS:
            emit_block(bt,out_uid,"sensing_loudness",parent_uid,"","","",0,0,0,0); break;
        case EXPR_USERNAME:
            emit_block(bt,out_uid,"sensing_username",parent_uid,"","","",0,0,0,0); break;
        case EXPR_ONLINE:
            emit_block(bt,out_uid,"sensing_online",parent_uid,"","","",0,0,0,0); break;
        case EXPR_DAYS_SINCE_2000:
            emit_block(bt,out_uid,"sensing_dayssince2000",parent_uid,"","","",0,0,0,0); break;
        case EXPR_TOUCHING_COLOR: {
            buf_printf(&inputs,"\"COLOR\":[1,[9,");
            buf_json_str(&inputs, e->str ? e->str : "#000000");
            buf_cat(&inputs,"]]");
            emit_block(bt,out_uid,"sensing_touchingcolor",parent_uid,"",inputs.buf,"",0,0,0,0);
            break;
        }
        case EXPR_COLOR_TOUCHING_COLOR: {
            buf_printf(&inputs,"\"COLOR\":[1,[9,");
            buf_json_str(&inputs, e->pair.a && e->pair.a->str ? e->pair.a->str : "#000000");
            buf_cat(&inputs,"]],\"COLOR2\":[1,[9,");
            buf_json_str(&inputs, e->pair.b && e->pair.b->str ? e->pair.b->str : "#000000");
            buf_cat(&inputs,"]]");
            emit_block(bt,out_uid,"sensing_coloristouchingcolor",parent_uid,"",inputs.buf,"",0,0,0,0);
            break;
        }
        case EXPR_CURRENT: {
            const char *which = e->str ? e->str : "year";
            const char *menu =
                strcmp(which,"year")==0        ? "YEAR"      :
                strcmp(which,"month")==0       ? "MONTH"     :
                strcmp(which,"date")==0        ? "DATE"      :
                strcmp(which,"day of week")==0 ? "DAYOFWEEK" :
                strcmp(which,"hour")==0        ? "HOUR"      :
                strcmp(which,"minute")==0      ? "MINUTE"    : "SECOND";
            buf_printf(&fields,"\"CURRENTMENU\":[\"%s\",null]", menu);
            emit_block(bt,out_uid,"sensing_current",parent_uid,"","",fields.buf,0,0,0,0);
            break;
        }
        case EXPR_SENSING_OF: {
            char muid[32]; new_uid(muid);
            Buf mf; buf_init(&mf);
            buf_printf(&mf,"\"OBJECT\":[\"%s\",null]", e->sensing_of.sprite ? e->sensing_of.sprite : "");
            emit_block(bt,muid,"sensing_of_object_menu",out_uid,"","",mf.buf,0,1,0,0);
            free(mf.buf);
            buf_printf(&inputs,"\"OBJECT\":[1,\"%s\"]", muid);
            buf_printf(&fields,"\"PROPERTY\":[\"%s\",null]", e->sensing_of.property ? e->sensing_of.property : "");
            emit_block(bt,out_uid,"sensing_of",parent_uid,"",inputs.buf,fields.buf,0,0,0,0);
            break;
        }

        /* ── list reporters ── */
        case EXPR_LIST_ITEM_NUM: {
            Buf idx; buf_init(&idx);
            expr_to_input(bt, e->list_item.index, "ITEM", out_uid, &idx);
            buf_cat(&inputs, idx.buf); free(idx.buf);
            buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]", e->list_item.list, e->list_item.list);
            emit_block(bt,out_uid,"data_itemnumoflist",parent_uid,"",inputs.buf,fields.buf,0,0,0,0);
            break;
        }

        default:
            emit_block(bt,out_uid,"operator_add",parent_uid,"","\"NUM1\":[1,[10,\"0\"]],\"NUM2\":[1,[10,\"0\"]]","",0,0,0,0);
            break;
    }
    free(inputs.buf); free(fields.buf);
}

/* ── helper: emit literal-or-reporter input ─────────────────────── */
static void input_expr(BlockTable *bt, Expr *e, const char *name,
                        const char *parent, Buf *out) {
    if (!e) { buf_printf(out, "\"%s\":[1,[10,\"\"]]", name); return; }
    if (e->type==EXPR_NUMBER || e->type==EXPR_STRING || e->type==EXPR_VAR) {
        expr_to_input(bt, e, name, parent, out);
    } else {
        char uid[32] = {0};
        emit_reporter(bt, e, parent, uid);
        buf_printf(out, "\"%s\":[3,\"%s\",[10,\"\"]]", name, uid);
    }
}

/* ── statement emitter ──────────────────────────────────────────── */
static void emit_stmt_chain(BlockTable *bt, Stmt **stmts, int count,
                             const char *parent_uid,
                             const char *forced_first_uid,
                             char *chain_first_uid) {
    if (count == 0) { chain_first_uid[0] = 0; return; }

    char **uids = malloc(count * sizeof(char*));
    for (int i = 0; i < count; i++) {
        uids[i] = malloc(32);
        if (i == 0 && forced_first_uid && forced_first_uid[0])
            strncpy(uids[i], forced_first_uid, 31);
        else
            new_uid(uids[i]);
        uids[i][31] = 0;
    }
    strcpy(chain_first_uid, uids[0]);

    for (int i = 0; i < count; i++) {
        Stmt *s = stmts[i];
        const char *uid    = uids[i];
        const char *next   = (i+1 < count) ? uids[i+1] : "";
        const char *parent = (i==0) ? parent_uid : uids[i-1];
        Buf inputs; buf_init(&inputs);
        Buf fields; buf_init(&fields);
        int first = 1;
#define ADDI(f) do { if(!first) buf_cat(&inputs,","); buf_cat(&inputs,(f).buf); free((f).buf); first=0; } while(0)

        switch (s->type) {
            case STMT_MOVE_STEPS: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"STEPS",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_movesteps",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_TURN_RIGHT: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"DEGREES",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_turnright",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_TURN_LEFT: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"DEGREES",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_turnleft",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_GOTO_XY: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"X",uid,&a); ADDI(a);
                Buf b; buf_init(&b); input_expr(bt,s->b,"Y",uid,&b); ADDI(b);
                emit_block(bt,uid,"motion_gotoxy",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_GOTO_TARGET: {
                const char *tgt = s->target;
                const char *val = strcmp(tgt,"random position")==0 ? "_random_" :
                                  strcmp(tgt,"mouse pointer")==0   ? "_mouse_"  : tgt;
                char muid[32]; new_uid(muid);
                char mfields[128];
                snprintf(mfields,sizeof(mfields),"\"TO\":[\"%s\",null]",val);
                emit_block(bt,muid,"motion_goto_menu",uid,"","",mfields,0,1,0,0);
                buf_printf(&inputs,"\"TO\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"motion_goto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_GLIDE_XY: {
                Buf t; buf_init(&t); input_expr(bt,s->secs,"SECS",uid,&t); ADDI(t);
                Buf a; buf_init(&a); input_expr(bt,s->a,"X",uid,&a); ADDI(a);
                Buf b; buf_init(&b); input_expr(bt,s->b,"Y",uid,&b); ADDI(b);
                emit_block(bt,uid,"motion_glidesecstoxy",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_GLIDE_TARGET: {
                const char *tgt = s->target;
                const char *val = strcmp(tgt,"random position")==0 ? "_random_" :
                                  strcmp(tgt,"mouse pointer")==0   ? "_mouse_"  : tgt;
                char muid[32]; new_uid(muid);
                char mfields[128]; snprintf(mfields,sizeof(mfields),"\"TO\":[\"%s\",null]",val);
                emit_block(bt,muid,"motion_glideto_menu",uid,"","",mfields,0,1,0,0);
                Buf t; buf_init(&t); input_expr(bt,s->secs,"SECS",uid,&t); ADDI(t);
                buf_printf(&inputs,",\"TO\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"motion_glideto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SET_X: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"X",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_setx",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SET_Y: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"Y",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_sety",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_CHANGE_X: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"DX",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_changexby",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_CHANGE_Y: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"DY",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_changeyby",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_POINT_DIR: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"DIRECTION",uid,&a); ADDI(a);
                emit_block(bt,uid,"motion_pointindirection",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_POINT_TOWARDS: {
                const char *tgt = s->target ? s->target : "";
                const char *val = strcmp(tgt,"mouse pointer")==0 ? "_mouse_" : tgt;
                char muid[32]; new_uid(muid);
                char mf[128]; snprintf(mf,sizeof(mf),"\"TOWARDS\":[\"%s\",null]",val);
                emit_block(bt,muid,"motion_pointtowards_menu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"TOWARDS\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"motion_pointtowards",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SAY: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"MESSAGE",uid,&a); ADDI(a);
                emit_block(bt,uid,"looks_say",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SAY_SECS: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"MESSAGE",uid,&a); ADDI(a);
                Buf t; buf_init(&t); input_expr(bt,s->secs,"SECS",uid,&t); ADDI(t);
                emit_block(bt,uid,"looks_sayforsecs",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_THINK: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"MESSAGE",uid,&a); ADDI(a);
                emit_block(bt,uid,"looks_think",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_THINK_SECS: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"MESSAGE",uid,&a); ADDI(a);
                Buf t; buf_init(&t); input_expr(bt,s->secs,"SECS",uid,&t); ADDI(t);
                emit_block(bt,uid,"looks_thinkforsecs",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SWITCH_COSTUME: {
                char muid[32]; new_uid(muid);
                char mf[256]; snprintf(mf,sizeof(mf),"\"COSTUME\":[\"%s\",null]",s->name?s->name:"");
                emit_block(bt,muid,"looks_costume",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"COSTUME\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"looks_switchcostumeto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_NEXT_COSTUME:
                emit_block(bt,uid,"looks_nextcostume",parent,next,"","",0,0,0,0);
                break;
            case STMT_SWITCH_BACKDROP: {
                char muid[32]; new_uid(muid);
                char mf[256]; snprintf(mf,sizeof(mf),"\"BACKDROP\":[\"%s\",null]",s->name?s->name:"");
                emit_block(bt,muid,"looks_backdrops",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"BACKDROP\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"looks_switchbackdropto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_NEXT_BACKDROP:
                emit_block(bt,uid,"looks_nextbackdrop",parent,next,"","",0,0,0,0);
                break;
            case STMT_SET_SIZE: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"SIZE",uid,&a); ADDI(a);
                emit_block(bt,uid,"looks_setsizeto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_CHANGE_SIZE: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"CHANGE",uid,&a); ADDI(a);
                emit_block(bt,uid,"looks_changesizeby",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SHOW: emit_block(bt,uid,"looks_show",parent,next,"","",0,0,0,0); break;
            case STMT_HIDE: emit_block(bt,uid,"looks_hide",parent,next,"","",0,0,0,0); break;
            case STMT_CHANGE_EFFECT: {
                char muid[32]; new_uid(muid);
                const char *effect = s->name ? s->name : "color";
                char mf[256]; snprintf(mf,sizeof(mf),"\"EFFECT\":[\"%s\",null]", effect);
                emit_block(bt,muid,"looks_effectmenu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"EFFECT\":[1,\"%s\"]",muid);
                first = 0;
                Buf a; buf_init(&a); input_expr(bt,s->a,"CHANGE",uid,&a); ADDI(a);
                emit_block(bt,uid,"looks_changeeffectby",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SET_EFFECT: {
                char muid[32]; new_uid(muid);
                const char *effect = s->name ? s->name : "color";
                char mf[256]; snprintf(mf,sizeof(mf),"\"EFFECT\":[\"%s\",null]", effect);
                emit_block(bt,muid,"looks_effectmenu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"EFFECT\":[1,\"%s\"]",muid);
                first = 0;
                Buf a; buf_init(&a); input_expr(bt,s->a,"VALUE",uid,&a); ADDI(a);
                emit_block(bt,uid,"looks_seteffectto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_CLEAR_EFFECTS:
                emit_block(bt,uid,"looks_cleargraphiceffects",parent,next,"","",0,0,0,0);
                break;
            case STMT_GOTO_FRONT_BACK: {
                const char *pos = s->name ? s->name : "front";
                char mf[256]; snprintf(mf,sizeof(mf),"\"FRONT_BACK\":[\"%s\",null]", pos);
                emit_block(bt,uid,"looks_gotofrontback",parent,next,"",mf,0,0,0,0);
                break;
            }
            case STMT_GOTO_LAYER: {
                char muid[32]; new_uid(muid);
                const char *dir = s->name ? s->name : "forward";
                char mf[256]; snprintf(mf,sizeof(mf),"\"FORWARD_BACKWARD\":[\"%s\",null]", dir);
                emit_block(bt,muid,"looks_forwardbackwardmenu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"FORWARD_BACKWARD\":[1,\"%s\"]",muid);
                first = 0;
                Buf a; buf_init(&a); input_expr(bt,s->a,"NUM",uid,&a); ADDI(a);
                emit_block(bt,uid,"looks_goforwardbackwardlayers",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_PLAY_SOUND: {
                char muid[32]; new_uid(muid);
                char mf[256]; snprintf(mf,sizeof(mf),"\"SOUND_MENU\":[\"%s\",null]",s->name?s->name:"");
                emit_block(bt,muid,"sound_sounds_menu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"SOUND_MENU\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"sound_play",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_PLAY_SOUND_UNTIL: {
                char muid[32]; new_uid(muid);
                char mf[256]; snprintf(mf,sizeof(mf),"\"SOUND_MENU\":[\"%s\",null]",s->name?s->name:"");
                emit_block(bt,muid,"sound_sounds_menu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"SOUND_MENU\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"sound_playuntildone",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_STOP_SOUNDS:
                emit_block(bt,uid,"sound_stopallsounds",parent,next,"","",0,0,0,0); break;
            case STMT_SET_VOLUME: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"VOLUME",uid,&a); ADDI(a);
                emit_block(bt,uid,"sound_setvolumeto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_CHANGE_VOLUME: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"VOLUME",uid,&a); ADDI(a);
                emit_block(bt,uid,"sound_changevolumeby",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_CHANGE_SOUND_EFFECT: {
                char muid[32]; new_uid(muid);
                const char *effect = s->name ? s->name : "pitch";
                char mf[256]; snprintf(mf,sizeof(mf),"\"EFFECT\":[\"%s\",null]", effect);
                emit_block(bt,muid,"sound_effectmenu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"EFFECT\":[1,\"%s\"]",muid);
                first = 0;
                Buf a; buf_init(&a); input_expr(bt,s->a,"VALUE",uid,&a); ADDI(a);
                emit_block(bt,uid,"sound_changeeffectby",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_SET_SOUND_EFFECT: {
                char muid[32]; new_uid(muid);
                const char *effect = s->name ? s->name : "pitch";
                char mf[256]; snprintf(mf,sizeof(mf),"\"EFFECT\":[\"%s\",null]", effect);
                emit_block(bt,muid,"sound_effectmenu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"EFFECT\":[1,\"%s\"]",muid);
                first = 0;
                Buf a; buf_init(&a); input_expr(bt,s->a,"VALUE",uid,&a); ADDI(a);
                emit_block(bt,uid,"sound_seteffectto",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_CLEAR_SOUND_EFFECTS:
                emit_block(bt,uid,"sound_cleareffects",parent,next,"","",0,0,0,0);
                break;
            case STMT_BROADCAST: {
                char muid[32]; new_uid(muid);
                char mf[256]; snprintf(mf,sizeof(mf),"\"BROADCAST_OPTION\":[\"%s\",\"%s\"]",
                    s->name?s->name:"",s->name?s->name:"");
                emit_block(bt,muid,"event_broadcast_menu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"BROADCAST_INPUT\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"event_broadcast",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_BROADCAST_WAIT: {
                char muid[32]; new_uid(muid);
                char mf[256]; snprintf(mf,sizeof(mf),"\"BROADCAST_OPTION\":[\"%s\",\"%s\"]",
                    s->name?s->name:"",s->name?s->name:"");
                emit_block(bt,muid,"event_broadcast_menu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"BROADCAST_INPUT\":[1,\"%s\"]",muid);
                emit_block(bt,uid,"event_broadcastandwait",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_WAIT: {
                Buf a; buf_init(&a); input_expr(bt,s->secs,"DURATION",uid,&a); ADDI(a);
                emit_block(bt,uid,"control_wait",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_WAIT_UNTIL: {
                char sub[32] = {0}; (void)sub;
                EMIT_CONDITION(bt,s->cond,uid,inputs);
                emit_block(bt,uid,"control_wait_until",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_FOREVER: {
                char body_first[32] = {0};
                if (s->body_count > 0) {
                    char buid[32] = {0};
                    new_uid(buid);
                    emit_stmt_chain(bt, s->body, s->body_count, uid, buid, body_first);
                }
                if (body_first[0]) buf_printf(&inputs,"\"SUBSTACK\":[2,\"%s\"]", body_first);
                emit_block(bt,uid,"control_forever",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_REPEAT: {
                Buf cnt; buf_init(&cnt); input_expr(bt,s->count,"TIMES",uid,&cnt); ADDI(cnt);
                char body_first[32] = {0};
                if (s->body_count > 0) {
                    char buid[32] = {0};
                    new_uid(buid);
                    emit_stmt_chain(bt, s->body, s->body_count, uid, buid, body_first);
                }
                if (body_first[0]) buf_printf(&inputs,",\"SUBSTACK\":[2,\"%s\"]", body_first);
                emit_block(bt,uid,"control_repeat",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_IF: {
                char sub[32] = {0}; (void)sub;
                EMIT_CONDITION(bt,s->cond,uid,inputs);
                char body_first[32] = {0};
                if (s->body_count > 0) {
                    char buid[32] = {0};
                    new_uid(buid);
                    emit_stmt_chain(bt, s->body, s->body_count, uid, buid, body_first);
                }
                if (body_first[0]) {
                    if (inputs.len) buf_cat(&inputs,",");
                    buf_printf(&inputs,"\"SUBSTACK\":[2,\"%s\"]", body_first);
                }
                emit_block(bt,uid,"control_if",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_IF_ELSE: {
                char sub[32] = {0}; (void)sub;
                EMIT_CONDITION(bt,s->cond,uid,inputs);
                char body_first[32] = {0};
                char else_first[32] = {0};
                if (s->body_count > 0) {
                    char buid[32] = {0};
                    new_uid(buid);
                    emit_stmt_chain(bt, s->body, s->body_count, uid, buid, body_first);
                }
                if (s->else_count > 0) {
                    char buid[32] = {0};
                    new_uid(buid);
                    emit_stmt_chain(bt, s->else_body, s->else_count, uid, buid, else_first);
                }
                if (body_first[0]) { if(inputs.len)buf_cat(&inputs,","); buf_printf(&inputs,"\"SUBSTACK\":[2,\"%s\"]",body_first); }
                if (else_first[0]) { if(inputs.len)buf_cat(&inputs,","); buf_printf(&inputs,"\"SUBSTACK2\":[2,\"%s\"]",else_first); }
                emit_block(bt,uid,"control_if_else",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_STOP: {
                const char *kind = s->name ? s->name : "all";
                const char *scratch_kind =
                    strcmp(kind,"all")==0                    ? "all" :
                    strcmp(kind,"this script")==0             ? "this script" :
                    strcmp(kind,"other scripts in sprite")==0 ? "other scripts in sprite" : "all";
                buf_printf(&fields,"\"STOP_OPTION\":[\"%s\",null]", scratch_kind);
                emit_block(bt,uid,"control_stop",parent,next,"",fields.buf,0,0,0,0);
                break;
            }
            case STMT_ASK: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"QUESTION",uid,&a); ADDI(a);
                emit_block(bt,uid,"sensing_askandwait",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_RESET_TIMER:
                emit_block(bt,uid,"sensing_resettimer",parent,next,"","",0,0,0,0); break;
            case STMT_SET_VAR: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"VALUE",uid,&a); ADDI(a);
                buf_printf(&fields,"\"VARIABLE\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_setvariableto",parent,next,inputs.buf,fields.buf,0,0,0,0);
                break;
            }
            case STMT_CHANGE_VAR: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"VALUE",uid,&a); ADDI(a);
                buf_printf(&fields,"\"VARIABLE\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_changevariableby",parent,next,inputs.buf,fields.buf,0,0,0,0);
                break;
            }
            case STMT_SHOW_VAR:
                buf_printf(&fields,"\"VARIABLE\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_showvariable",parent,next,"",fields.buf,0,0,0,0); break;
            case STMT_HIDE_VAR:
                buf_printf(&fields,"\"VARIABLE\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_hidevariable",parent,next,"",fields.buf,0,0,0,0); break;
            case STMT_LIST_ADD: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"ITEM",uid,&a); ADDI(a);
                buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_addtolist",parent,next,inputs.buf,fields.buf,0,0,0,0);
                break;
            }
            case STMT_LIST_DELETE: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"INDEX",uid,&a); ADDI(a);
                buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_deleteoflist",parent,next,inputs.buf,fields.buf,0,0,0,0);
                break;
            }
            case STMT_LIST_DELETE_ALL:
                buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_deletealloflist",parent,next,"",fields.buf,0,0,0,0); break;
            case STMT_LIST_INSERT: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"ITEM",uid,&a); ADDI(a);
                Buf b; buf_init(&b); input_expr(bt,s->b,"INDEX",uid,&b); ADDI(b);
                buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_insertatlist",parent,next,inputs.buf,fields.buf,0,0,0,0);
                break;
            }
            case STMT_LIST_REPLACE: {
                Buf a; buf_init(&a); input_expr(bt,s->a,"INDEX",uid,&a); ADDI(a);
                Buf b; buf_init(&b); input_expr(bt,s->b,"ITEM",uid,&b); ADDI(b);
                buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_replaceitemoflist",parent,next,inputs.buf,fields.buf,0,0,0,0);
                break;
            }
            case STMT_SHOW_LIST:
                buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_showlist",parent,next,"",fields.buf,0,0,0,0); break;
            case STMT_HIDE_LIST:
                buf_printf(&fields,"\"LIST\":[\"%s\",\"%s\"]",s->name,s->name);
                emit_block(bt,uid,"data_hidelist",parent,next,"",fields.buf,0,0,0,0); break;
            case STMT_REPEAT_UNTIL: {
                char sub[32] = {0}; (void)sub;
                if (s->cond) {
                    EMIT_CONDITION(bt,s->cond,uid,inputs);
                }
                char body_first[32] = {0};
                if (s->body_count > 0) {
                    char buid[32] = {0};
                    new_uid(buid);
                    emit_stmt_chain(bt, s->body, s->body_count, uid, buid, body_first);
                }
                if (body_first[0]) { if(inputs.len)buf_cat(&inputs,","); buf_printf(&inputs,"\"SUBSTACK\":[2,\"%s\"]",body_first); }
                emit_block(bt,uid,"control_repeat_until",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_PROC_CALL: {
                Buf arg_ids;   buf_init(&arg_ids);   buf_cat(&arg_ids, "[");
                Buf arg_names; buf_init(&arg_names); buf_cat(&arg_names, "[");
                Buf arg_defs;  buf_init(&arg_defs);  buf_cat(&arg_defs, "[");
                int first2 = 1;
                for (int ai = 0; ai < s->arg_count; ai++) {
                    char auid[32] = {0};
                    new_uid(auid);
                    if (!first2) {
                        buf_cat(&arg_ids,   ",");
                        buf_cat(&arg_names, ",");
                        buf_cat(&arg_defs,  ",");
                    }
                    Buf aidq; buf_init(&aidq);
                    buf_printf(&aidq, "\\\"%s\\\"", auid);
                    buf_cat(&arg_ids, aidq.buf); free(aidq.buf);
                    Buf ainp; buf_init(&ainp);
                    input_expr(bt, s->args[ai], auid, uid, &ainp);
                    if (inputs.len) buf_cat(&inputs, ",");
                    buf_cat(&inputs, ainp.buf); free(ainp.buf);
                    first2 = 0;
                }
                buf_cat(&arg_ids,   "]");
                buf_cat(&arg_names, "]");
                buf_cat(&arg_defs,  "]");

                Buf mut; buf_init(&mut);
                buf_printf(&mut, "\"tagName\":\"mutation\",\"children\":[],\"proccode\":");
                Buf pc; buf_init(&pc);
                buf_cat(&pc, "\"");
                const char *pn = s->name ? s->name : "";
                for (; *pn; pn++) {
                    if (*pn == '"' || *pn == '\\') buf_cat(&pc, "\\");
                    char ch[2] = {*pn, 0};
                    buf_cat(&pc, ch);
                }
                for (int ai = 0; ai < s->arg_count; ai++) buf_cat(&pc, " %s");
                buf_cat(&pc, "\"");
                buf_cat(&mut, pc.buf); free(pc.buf);
                buf_printf(&mut, ",\"argumentids\":\"%s\"", arg_ids.buf);
                buf_printf(&mut, ",\"warp\":%s", "false");
                free(arg_ids.buf); free(arg_names.buf); free(arg_defs.buf);

                {
                    Buf blk; buf_init(&blk);
                    buf_printf(&blk, "{\"opcode\":\"procedures_call\"");
                    if (next && next[0])   buf_printf(&blk, ",\"next\":\"%s\"",   next);
                    else           buf_printf(&blk, ",\"next\":null");
                    if (parent && parent[0]) buf_printf(&blk, ",\"parent\":\"%s\"", parent);
                    else           buf_printf(&blk, ",\"parent\":null");
                    buf_printf(&blk, ",\"inputs\":{%s}", inputs.buf);
                    buf_printf(&blk, ",\"fields\":{}");
                    buf_printf(&blk, ",\"shadow\":false,\"topLevel\":false");
                    buf_printf(&blk, ",\"mutation\":{%s}}", mut.buf);
                    btable_add(bt, uid, blk.buf);
                    free(blk.buf); free(mut.buf);
                }
                free(inputs.buf); free(fields.buf);
                goto next_stmt;
            }
            /* ── motion extras ── */
            case STMT_IF_ON_EDGE_BOUNCE:
                emit_block(bt,uid,"motion_ifonedgebounce",parent,next,"","",0,0,0,0); break;
            case STMT_SET_ROTATION_STYLE: {
                buf_printf(&fields,"\"STYLE\":[\"%s\",null]", s->name ? s->name : "all around");
                emit_block(bt,uid,"motion_setrotationstyle",parent,next,"",fields.buf,0,0,0,0);
                break;
            }

            /* ── clone control ── */
            case STMT_CREATE_CLONE: {
                const char *tgt = s->target ? s->target : "_myself_";
                char muid[32]; new_uid(muid);
                char mf[256]; snprintf(mf,sizeof(mf),"\"CLONE_OPTION\":[\"%s\",null]", tgt);
                emit_block(bt,muid,"control_create_clone_of_menu",uid,"","",mf,0,1,0,0);
                buf_printf(&inputs,"\"CLONE_OPTION\":[1,\"%s\"]", muid);
                emit_block(bt,uid,"control_create_clone_of",parent,next,inputs.buf,"",0,0,0,0);
                break;
            }
            case STMT_DELETE_CLONE:
                emit_block(bt,uid,"control_delete_this_clone",parent,next,"","",0,0,0,0); break;

            /* ── sensing ── */
            case STMT_SET_DRAG_MODE: {
                buf_printf(&fields,"\"DRAG_MODE\":[\"%s\",null]", s->name ? s->name : "draggable");
                emit_block(bt,uid,"sensing_setdragmode",parent,next,"",fields.buf,0,0,0,0);
                break;
            }

            default:
                fprintf(stderr,"emitter: unhandled stmt type %d\n", s->type);
                break;
        }
        free(inputs.buf); free(fields.buf);
        next_stmt:;
    }
    for (int i=0;i<count;i++) free(uids[i]);
    free(uids);
}

/* ── hat block emitter ──────────────────────────────────────────── */
static void emit_script(BlockTable *bt, Script *sc, double x, double y) {
    char hat_uid[32] = {0};
    new_uid(hat_uid);
    Buf inputs; buf_init(&inputs);
    Buf fields;  buf_init(&fields);

    /* HAT_NONE: floating script — emit body with hat_uid as forced first block.
       The first block must be topLevel:true with coordinates so Scratch renders it. */
    if (sc->hat == HAT_NONE) {
        if (sc->body_count > 0) {
            char first_uid[32] = {0};
            emit_stmt_chain(bt, sc->body, sc->body_count, "", hat_uid, first_uid);
            /* Patch the first block to be top-level with position */
            if (first_uid[0]) {
                for (int ei = 0; ei < bt->count; ei++) {
                    if (strcmp(bt->entries[ei].uid, first_uid) == 0) {
                        char *old = bt->entries[ei].json;
                        /* Replace "topLevel":false with "topLevel":true and append x/y */
                        char *pos = strstr(old, "\"topLevel\":false");
                        if (pos) {
                            Buf patched; buf_init(&patched);
                            int prefix_len = (int)(pos - old);
                            /* write everything before the match */
                            buf_ensure(&patched, prefix_len);
                            memcpy(patched.buf + patched.len, old, prefix_len);
                            patched.len += prefix_len;
                            patched.buf[patched.len] = 0;
                            /* write replacement */
                            buf_printf(&patched, "\"topLevel\":true,\"x\":%.0f,\"y\":%.0f", x, y);
                            /* write everything after the match */
                            buf_cat(&patched, pos + strlen("\"topLevel\":false"));
                            free(old);
                            bt->entries[ei].json = patched.buf;
                        }
                        break;
                    }
                }
            }
        }
        free(inputs.buf); free(fields.buf);
        return;
    }

    /* For all hat types: emit body with hat_uid as parent */
    char body_first[32] = {0};
    if (sc->body_count > 0) {
        char buid[32] = {0};
        new_uid(buid);
        emit_stmt_chain(bt, sc->body, sc->body_count, hat_uid, buid, body_first);
    }

    if (sc->hat == HAT_PROCEDURE_DEF) {
        Buf proccode; buf_init(&proccode);
        if (sc->hat_arg) buf_cat(&proccode, sc->hat_arg);
        for (int i = 0; i < sc->proc_param_count; i++) buf_cat(&proccode, " %s");

        Buf arg_ids_esc;  buf_init(&arg_ids_esc);
        Buf arg_names_esc; buf_init(&arg_names_esc);
        Buf arg_defs_esc;  buf_init(&arg_defs_esc);
        buf_cat(&arg_ids_esc, "[");
        buf_cat(&arg_names_esc, "[");
        buf_cat(&arg_defs_esc,  "[");

        for (int i = 0; i < sc->proc_param_count; i++) {
            char puid[32] = {0};
            new_uid(puid);
            Buf pf; buf_init(&pf);
            buf_printf(&pf, "\"VALUE\":[\"%s\",null]", sc->proc_params[i]);
            emit_block(bt, puid, "argument_reporter_string_number", hat_uid, "", "", pf.buf, 0, 1, 0, 0);
            free(pf.buf);
            if (i > 0) {
                buf_cat(&arg_ids_esc,   ",");
                buf_cat(&arg_names_esc, ",");
                buf_cat(&arg_defs_esc,  ",");
            }
            buf_printf(&arg_ids_esc,   "\\\"%s\\\"", puid);
            buf_printf(&arg_names_esc, "\\\"%s\\\"", sc->proc_params[i]);
            buf_cat(&arg_defs_esc, "\\\"\\\"");
            if (inputs.len) buf_cat(&inputs, ",");
            buf_printf(&inputs, "\"%s\":[1,\"%s\"]", puid, puid);
        }
        buf_cat(&arg_ids_esc,   "]");
        buf_cat(&arg_names_esc, "]");
        buf_cat(&arg_defs_esc,  "]");

        char proto_uid[32] = {0};
        new_uid(proto_uid);
        {
            Buf pmut; buf_init(&pmut);
            buf_printf(&pmut, "\"tagName\":\"mutation\",\"children\":[],\"proccode\":");
            buf_json_str(&pmut, proccode.buf);
            buf_printf(&pmut, ",\"argumentids\":\"%s\"", arg_ids_esc.buf);
            buf_printf(&pmut, ",\"argumentnames\":\"%s\"", arg_names_esc.buf);
            buf_printf(&pmut, ",\"argumentdefaults\":\"%s\"", arg_defs_esc.buf);
            buf_printf(&pmut, ",\"warp\":%s", sc->no_refresh ? "true" : "false");

            Buf pblk; buf_init(&pblk);
            buf_printf(&pblk, "{\"opcode\":\"procedures_prototype\"");
            buf_printf(&pblk, ",\"next\":null,\"parent\":\"%s\"", hat_uid);
            buf_printf(&pblk, ",\"inputs\":{%s}", inputs.buf);
            buf_printf(&pblk, ",\"fields\":{}");
            buf_printf(&pblk, ",\"shadow\":true,\"topLevel\":false");
            buf_printf(&pblk, ",\"mutation\":{%s}}", pmut.buf);
            btable_add(bt, proto_uid, pblk.buf);
            free(pmut.buf); free(pblk.buf);
        }
        free(arg_ids_esc.buf); free(arg_names_esc.buf); free(arg_defs_esc.buf);

        {
            Buf defmut; buf_init(&defmut);
            buf_printf(&defmut, "\"tagName\":\"mutation\",\"children\":[],\"proccode\":");
            buf_json_str(&defmut, proccode.buf);
            buf_printf(&defmut, ",\"warp\":%s", sc->no_refresh ? "true" : "false");

            Buf def_inputs; buf_init(&def_inputs);
            buf_printf(&def_inputs, "\"custom_block\":[1,\"%s\"]", proto_uid);

            Buf blk; buf_init(&blk);
            buf_printf(&blk, "{\"opcode\":\"procedures_definition\"");
            if (body_first[0])
                buf_printf(&blk, ",\"next\":\"%s\"", body_first);
            else
                buf_printf(&blk, ",\"next\":null");
            buf_printf(&blk, ",\"parent\":null");
            buf_printf(&blk, ",\"inputs\":{%s}", def_inputs.buf);
            buf_printf(&blk, ",\"fields\":{}");
            buf_printf(&blk, ",\"shadow\":false,\"topLevel\":true");
            buf_printf(&blk, ",\"x\":%.0f,\"y\":%.0f", x, y);
            buf_printf(&blk, ",\"mutation\":{%s}}", defmut.buf);
            btable_add(bt, hat_uid, blk.buf);
            free(defmut.buf); free(def_inputs.buf); free(blk.buf);
        }
        free(proccode.buf);
        free(inputs.buf); free(fields.buf);
        return;
    }

    /* ── new hats that need special handling ── */
    if (sc->hat == HAT_CLONE_START) {
        emit_block(bt, hat_uid, "control_start_as_clone", "", body_first,
                   "", "", 1, 0, x, y);
        free(inputs.buf); free(fields.buf);
        return;
    }
    if (sc->hat == HAT_BACKDROP_SWITCHES) {
        buf_printf(&fields,"\"BACKDROP\":[\"%s\",null]", sc->hat_arg ? sc->hat_arg : "");
        emit_block(bt, hat_uid, "event_whenbackdropswitchesto", "", body_first,
                   "", fields.buf, 1, 0, x, y);
        free(inputs.buf); free(fields.buf);
        return;
    }
    if (sc->hat == HAT_GREATER_THAN) {
        if (sc->hat_threshold) {
            Buf thr; buf_init(&thr);
            expr_to_input(bt, sc->hat_threshold, "VALUE", hat_uid, &thr);
            buf_cat(&inputs, thr.buf); free(thr.buf);
        } else {
            buf_printf(&inputs,"\"VALUE\":[1,[4,\"10\"]]");
        }
        buf_printf(&fields,"\"WHENGREATERTHANMENU\":[\"%s\",null]",
                   sc->hat_arg ? sc->hat_arg : "LOUDNESS");
        emit_block(bt, hat_uid, "event_whengreaterthan", "", body_first,
                   inputs.buf, fields.buf, 1, 0, x, y);
        free(inputs.buf); free(fields.buf);
        return;
    }

    const char *opcode = "event_whenflagclicked";
    switch (sc->hat) {
        case HAT_GREEN_FLAG:    opcode = "event_whenflagclicked"; break;
        case HAT_MESSAGE:       opcode = "event_whenbroadcastreceived"; break;
        case HAT_KEY_PRESSED:   opcode = "event_whenkeypressed"; break;
        case HAT_SPRITE_CLICKED:opcode = "event_whenthisspriteclicked"; break;
        default: break;
    }

    if (sc->hat == HAT_MESSAGE && sc->hat_arg) {
        buf_printf(&fields,"\"BROADCAST_OPTION\":[\"%s\",\"%s\"]",sc->hat_arg,sc->hat_arg);
    }
    if (sc->hat == HAT_KEY_PRESSED && sc->hat_arg) {
        buf_printf(&fields,"\"KEY_OPTION\":[\"%s\",null]",sc->hat_arg);
    }

    emit_block(bt, hat_uid, opcode, "", body_first,
               inputs.buf, fields.buf, 1, 0, x, y);

    free(inputs.buf); free(fields.buf);
}

/* ── blank costume ──────────────────────────────────────────────── */
static const char BLANK_PNG_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwADhQGAWjR9awAAAABJRU5ErkJggg==";

/* ── sprite JSON ────────────────────────────────────────────────── */
static void emit_sprite_json(Buf *out, Sprite *sp, Program *prog,
                              int is_stage, double x_start, double *y_offset,
                              const StateTable *st) {
    BlockTable bt;
    btable_init(&bt);

    double sx = 0, sy = 0;
    for (int i = 0; i < sp->script_count; i++) {
        int blocks_before = bt.count;
        emit_script(&bt, sp->scripts[i], sx, sy);
        /* Only advance y if something was actually emitted */
        if (bt.count > blocks_before)
            sy += 200;
    }

    /* variables */
    buf_cat(out, "\"variables\":{");
    int first = 1;
    #define EMIT_VAR(v) do { \
        if (!first) buf_cat(out, ","); \
        const char *_val = state_get_var(st, (v)->name); \
        buf_json_str(out, (v)->name); buf_cat(out, ":["); \
        buf_json_str(out, (v)->name); buf_cat(out, ","); \
        buf_json_val(out, _val ? _val : "0"); \
        buf_cat(out, "]"); \
        first = 0; \
    } while(0)
    if (is_stage) {
        for (int i = 0; i < prog->global_count; i++) EMIT_VAR(prog->globals[i]);
        for (int si = 0; si < prog->sprite_count; si++) {
            Sprite *s2 = prog->sprites[si];
            for (int vi = 0; vi < s2->var_count; vi++)
                if (s2->vars[vi]->is_global) EMIT_VAR(s2->vars[vi]);
        }
    } else {
        for (int i = 0; i < sp->var_count; i++)
            if (!sp->vars[i]->is_global) EMIT_VAR(sp->vars[i]);
    }
    #undef EMIT_VAR
    buf_cat(out, "},");

    /* lists */
    buf_cat(out, "\"lists\":{");
    first = 1;
    #define EMIT_LIST(l) do { \
        if (!first) buf_cat(out, ","); \
        const StateList *_sl = state_get_list(st, (l)->name); \
        buf_json_str(out, (l)->name); buf_cat(out, ":["); \
        buf_json_str(out, (l)->name); buf_cat(out, ",["); \
        if (_sl) { \
            for (int _i = 0; _i < _sl->count; _i++) { \
                if (_i) buf_cat(out, ","); \
                buf_json_val(out, _sl->items[_i]); \
            } \
        } \
        buf_cat(out, "]]"); \
        first = 0; \
    } while(0)
    if (is_stage) {
        for (int i = 0; i < prog->global_list_count; i++) EMIT_LIST(prog->global_lists[i]);
        for (int si = 0; si < prog->sprite_count; si++) {
            Sprite *s2 = prog->sprites[si];
            for (int li = 0; li < s2->list_count; li++)
                if (s2->lists[li]->is_global) EMIT_LIST(s2->lists[li]);
        }
    } else {
        for (int i = 0; i < sp->list_count; i++)
            if (!sp->lists[i]->is_global) EMIT_LIST(sp->lists[i]);
    }
    #undef EMIT_LIST
    buf_cat(out, "},");

    buf_cat(out, "\"broadcasts\":{},");

    /* blocks */
    buf_cat(out, "\"blocks\":{");
    for (int i = 0; i < bt.count; i++) {
        if (i) buf_cat(out, ",");
        buf_json_str(out, bt.entries[i].uid);
        buf_cat(out, ":");
        buf_cat(out, bt.entries[i].json);
        free(bt.entries[i].json);
    }
    free(bt.entries);
    buf_cat(out, "},");

    buf_cat(out, "\"comments\":{},\"currentCostume\":0,\"sounds\":[],");

    buf_printf(out,
        "\"costumes\":[{\"assetId\":\"bcf454acf82e4f0b1f90e8b3d7c16c7d\","
        "\"name\":\"blank\","
        "\"bitmapResolution\":1,"
        "\"dataFormat\":\"png\","
        "\"md5ext\":\"bcf454acf82e4f0b1f90e8b3d7c16c7d.png\","
        "\"rotationCenterX\":0,\"rotationCenterY\":0}],");

    (void)x_start; (void)y_offset; (void)BLANK_PNG_B64;
}

/* ── project.json ───────────────────────────────────────────────── */
int emit_project_json_with_state(Program *prog, char **buf_out, const StateTable *st) {
    Buf out;
    buf_init(&out);

    buf_cat(&out, "{\"targets\":[");

    Sprite *stage = NULL;
    for (int i = 0; i < prog->sprite_count; i++)
        if (prog->sprites[i]->is_stage) { stage = prog->sprites[i]; break; }

    buf_cat(&out, "{\"isStage\":true,\"name\":\"Stage\",\"layerOrder\":0,");
    buf_cat(&out, "\"tempo\":60,\"videoTransparency\":50,\"videoState\":\"on\",");
    buf_cat(&out, "\"textToSpeechLanguage\":null,");
    if (!stage) {
        stage = sprite_new(NULL, 1);
    }
    emit_sprite_json(&out, stage, prog, 1, 0, NULL, st);
    buf_cat(&out, "\"volume\":100,\"visible\":true,\"x\":0,\"y\":0,\"size\":100,"
                  "\"direction\":90,\"draggable\":false,\"rotationStyle\":\"all around\"}");

    int layer = 1;
    for (int i = 0; i < prog->sprite_count; i++) {
        Sprite *sp = prog->sprites[i];
        if (sp->is_stage) continue;
        buf_cat(&out, ",{\"isStage\":false,");
        buf_cat(&out, "\"name\":"); buf_json_str(&out, sp->name ? sp->name : "Sprite");
        buf_printf(&out, ",\"layerOrder\":%d,", layer++);
        emit_sprite_json(&out, sp, prog, 0, (i+1)*200.0, NULL, st);
        buf_printf(&out,
            "\"volume\":100,\"visible\":true,\"x\":%.0f,\"y\":0,\"size\":100,"
            "\"direction\":90,\"draggable\":false,\"rotationStyle\":\"all around\"}",
            (double)((i)*150 - 150));
    }

    buf_cat(&out, "],");
    buf_cat(&out, "\"monitors\":[],\"extensions\":[],\"meta\":{\"semver\":\"3.0.0\","
                  "\"vm\":\"0.2.0\",\"agent\":\"jappl2sb3\"}}");

    *buf_out = out.buf;
    return 0;
}

int emit_project_json(Program *prog, char **buf_out) {
    return emit_project_json_with_state(prog, buf_out, NULL);
}

/* ── zip writer ──────────────────────────────────────────────────── */
static unsigned int crc32_calc(const unsigned char *data, int len) {
    static unsigned int table[256];
    static int built = 0;
    if (!built) {
        for (int i=0;i<256;i++){
            unsigned int c=i;
            for (int j=0;j<8;j++) c=(c&1)?(0xEDB88320^(c>>1)):(c>>1);
            table[i]=c;
        }
        built=1;
    }
    unsigned int crc=0xFFFFFFFF;
    for (int i=0;i<len;i++) crc=table[(crc^data[i])&0xFF]^(crc>>8);
    return crc^0xFFFFFFFF;
}

static int b64decode(const char *src, unsigned char *dst) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int len=0;
    for (; *src; src+=4) {
        if (!src[1]) break;
        int a=strchr(t,src[0])-t, b=strchr(t,src[1])-t;
        int c=src[2]=='='?0:strchr(t,src[2])-t;
        int d=src[3]=='='?0:strchr(t,src[3])-t;
        dst[len++]=(a<<2)|(b>>4);
        if (src[2]!='=') dst[len++]=(b<<4)|(c>>2);
        if (src[3]!='=') dst[len++]=(c<<6)|d;
    }
    return len;
}

typedef struct { unsigned char *data; int len; int cap; } ByteBuf;
static void bb_init(ByteBuf *b) { b->data=malloc(65536); b->cap=65536; b->len=0; }
static void bb_ensure(ByteBuf *b, int n) {
    while (b->len+n>=b->cap){ b->cap*=2; b->data=realloc(b->data,b->cap); }
}
static void bb_write(ByteBuf *b, const void *d, int n) {
    bb_ensure(b,n); memcpy(b->data+b->len,d,n); b->len+=n;
}
static void bb_u16le(ByteBuf *b, unsigned short v) {
    unsigned char t[2]={v&0xFF,(v>>8)&0xFF}; bb_write(b,t,2);
}
static void bb_u32le(ByteBuf *b, unsigned int v) {
    unsigned char t[4]={v&0xFF,(v>>8)&0xFF,(v>>16)&0xFF,(v>>24)&0xFF}; bb_write(b,t,4);
}

static int zip_add_file(ByteBuf *z, const char *name,
                         const unsigned char *data, int dlen,
                         unsigned int *crc_out) {
    unsigned int crc = crc32_calc(data, dlen);
    if (crc_out) *crc_out = crc;
    int offset = z->len;
    int nlen = strlen(name);

    /* local file header */
    bb_write(z, "\x50\x4b\x03\x04", 4);
    bb_u16le(z, 20);     /* version needed */
    bb_u16le(z, 0);      /* flags */
    bb_u16le(z, 0);      /* compression: stored */
    bb_u16le(z, 0);      /* mod time */
    bb_u16le(z, 0);      /* mod date */
    bb_u32le(z, crc);
    bb_u32le(z, dlen);   /* compressed size */
    bb_u32le(z, dlen);   /* uncompressed size */
    bb_u16le(z, nlen);
    bb_u16le(z, 0);      /* extra len */
    bb_write(z, name, nlen);
    bb_write(z, data, dlen);
    return offset;
}

int emit_sb3_with_state(Program *prog, const char *path, const StateTable *st) {
    char *json = NULL;
    if (emit_project_json_with_state(prog, &json, st) != 0) return -1;

    unsigned char png[256];
    int png_len = b64decode(BLANK_PNG_B64, png);

    ByteBuf z; bb_init(&z);

    typedef struct { int offset; unsigned int crc; int dlen; char name[64]; } CDEntry;
    CDEntry entries[2];
    int ne = 0;

    entries[ne].offset = zip_add_file(&z, "project.json",
        (const unsigned char*)json, strlen(json), &entries[ne].crc);
    entries[ne].dlen = strlen(json);
    strcpy(entries[ne].name, "project.json");
    ne++;

    entries[ne].offset = zip_add_file(&z, "bcf454acf82e4f0b1f90e8b3d7c16c7d.png",
        png, png_len, &entries[ne].crc);
    entries[ne].dlen = png_len;
    strcpy(entries[ne].name, "bcf454acf82e4f0b1f90e8b3d7c16c7d.png");
    ne++;

    int cd_start = z.len;
    for (int i = 0; i < ne; i++) {
        int nlen = strlen(entries[i].name);
        bb_write(&z, "\x50\x4b\x01\x02", 4);
        bb_u16le(&z, 20);  /* version made by */
        bb_u16le(&z, 20);  /* version needed */
        bb_u16le(&z, 0);   /* flags */
        bb_u16le(&z, 0);   /* compression */
        bb_u16le(&z, 0);   /* mod time */
        bb_u16le(&z, 0);   /* mod date */
        bb_u32le(&z, entries[i].crc);
        bb_u32le(&z, entries[i].dlen);
        bb_u32le(&z, entries[i].dlen);
        bb_u16le(&z, nlen);
        bb_u16le(&z, 0);   /* extra */
        bb_u16le(&z, 0);   /* comment */
        bb_u16le(&z, 0);   /* disk start */
        bb_u16le(&z, 0);   /* int attrib */
        bb_u32le(&z, 0);   /* ext attrib */
        bb_u32le(&z, entries[i].offset);
        bb_write(&z, entries[i].name, nlen);
    }
    int cd_size = z.len - cd_start;

    bb_write(&z, "\x50\x4b\x05\x06", 4);
    bb_u16le(&z, 0);   /* disk number */
    bb_u16le(&z, 0);   /* disk with cd */
    bb_u16le(&z, ne);  /* entries this disk */
    bb_u16le(&z, ne);  /* total entries */
    bb_u32le(&z, cd_size);
    bb_u32le(&z, cd_start);
    bb_u16le(&z, 0);   /* comment length */

    FILE *f = fopen(path, "wb");
    if (!f) { free(json); free(z.data); return -1; }
    fwrite(z.data, 1, z.len, f);
    fclose(f);

    free(json);
    free(z.data);
    return 0;
}

int emit_sb3(Program *prog, const char *path) {
    return emit_sb3_with_state(prog, path, NULL);
}
