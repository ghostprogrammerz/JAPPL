#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "decompiler.h"

/* ── minimal JSON parser ─────────────────────────────────────────── */
/* We only need to navigate Scratch's project.json.
   We implement a simple recursive-descent JSON reader that builds
   a tree of JVal nodes. */

typedef enum { JNull, JBool, JNum, JStr, JArr, JObj } JType;
typedef struct JVal JVal;
typedef struct JKV  JKV;

struct JKV { char *key; JVal *val; };
struct JVal {
    JType type;
    union {
        int    boolean;
        double number;
        char  *string;
        struct { JVal **items; int count; } arr;
        struct { JKV  *pairs;  int count; } obj;
    };
};

static JVal *jval_new(JType t) { JVal *v=calloc(1,sizeof(JVal)); v->type=t; return v; }

/* ── JSON tokenizer (inline, no separate lexer needed) ───────────── */
typedef struct { const char *s; int pos; } JP;

static void jp_skip(JP *p) {
    while (p->s[p->pos] && (p->s[p->pos]==' '||p->s[p->pos]=='\t'||
           p->s[p->pos]=='\n'||p->s[p->pos]=='\r')) p->pos++;
}

static char *jp_str(JP *p) {
    /* assumes current char is '"' */
    p->pos++; /* skip opening " */
    int start = p->pos;
    /* first pass: measure */
    int len = 0;
    for (int i = start; p->s[i] && p->s[i] != '"'; i++) {
        if (p->s[i] == '\\') i++;
        len++;
    }
    char *out = malloc(len + 1);
    int j = 0;
    while (p->s[p->pos] && p->s[p->pos] != '"') {
        if (p->s[p->pos] == '\\') {
            p->pos++;
            switch (p->s[p->pos]) {
                case '"':  out[j++]='"';  break;
                case '\\': out[j++]='\\'; break;
                case 'n':  out[j++]='\n'; break;
                case 't':  out[j++]='\t'; break;
                case 'r':  out[j++]='\r'; break;
                default:   out[j++]=p->s[p->pos]; break;
            }
        } else {
            out[j++] = p->s[p->pos];
        }
        p->pos++;
    }
    out[j] = '\0';
    if (p->s[p->pos] == '"') p->pos++; /* skip closing " */
    return out;
}

static JVal *jp_parse(JP *p);

static JVal *jp_arr(JP *p) {
    p->pos++; /* skip [ */
    JVal *v = jval_new(JArr);
    v->arr.items = malloc(64*sizeof(JVal*));
    v->arr.count = 0;
    int cap = 64;
    jp_skip(p);
    if (p->s[p->pos] == ']') { p->pos++; return v; }
    while (1) {
        if (v->arr.count >= cap) { cap*=2; v->arr.items=realloc(v->arr.items,cap*sizeof(JVal*)); }
        v->arr.items[v->arr.count++] = jp_parse(p);
        jp_skip(p);
        if (p->s[p->pos] == ',') { p->pos++; jp_skip(p); continue; }
        break;
    }
    if (p->s[p->pos] == ']') p->pos++;
    return v;
}

static JVal *jp_obj(JP *p) {
    p->pos++; /* skip { */
    JVal *v = jval_new(JObj);
    v->obj.pairs = malloc(64*sizeof(JKV));
    v->obj.count = 0;
    int cap = 64;
    jp_skip(p);
    if (p->s[p->pos] == '}') { p->pos++; return v; }
    while (1) {
        jp_skip(p);
        if (p->s[p->pos] != '"') break;
        if (v->obj.count >= cap) { cap*=2; v->obj.pairs=realloc(v->obj.pairs,cap*sizeof(JKV)); }
        char *key = jp_str(p);
        jp_skip(p);
        if (p->s[p->pos] == ':') p->pos++;
        jp_skip(p);
        JVal *val = jp_parse(p);
        v->obj.pairs[v->obj.count].key = key;
        v->obj.pairs[v->obj.count].val = val;
        v->obj.count++;
        jp_skip(p);
        if (p->s[p->pos] == ',') { p->pos++; jp_skip(p); continue; }
        break;
    }
    if (p->s[p->pos] == '}') p->pos++;
    return v;
}

static JVal *jp_parse(JP *p) {
    jp_skip(p);
    char c = p->s[p->pos];
    if (c == '"') {
        JVal *v = jval_new(JStr);
        v->string = jp_str(p);
        return v;
    }
    if (c == '[') return jp_arr(p);
    if (c == '{') return jp_obj(p);
    if (c == 't') { p->pos+=4; JVal *v=jval_new(JBool); v->boolean=1; return v; }
    if (c == 'f') { p->pos+=5; JVal *v=jval_new(JBool); v->boolean=0; return v; }
    if (c == 'n') { p->pos+=4; return jval_new(JNull); }
    /* number */
    int start=p->pos;
    if (c=='-') p->pos++;
    while (p->s[p->pos]&&(p->s[p->pos]>='0'&&p->s[p->pos]<='9'||p->s[p->pos]=='.'||
           p->s[p->pos]=='e'||p->s[p->pos]=='E'||p->s[p->pos]=='+'||p->s[p->pos]=='-'))
        p->pos++;
    JVal *v = jval_new(JNum);
    v->number = atof(p->s + start);
    return v;
}

/* ── JSON accessors ─────────────────────────────────────────────── */
static JVal *jobj_get(JVal *obj, const char *key) {
    if (!obj || obj->type != JObj) return NULL;
    for (int i=0;i<obj->obj.count;i++)
        if (strcmp(obj->obj.pairs[i].key, key)==0)
            return obj->obj.pairs[i].val;
    return NULL;
}
static const char *jstr(JVal *v) { return (v&&v->type==JStr)?v->string:""; }
static double jnum(JVal *v) { return (v&&v->type==JNum)?v->number:0; }
static JVal *jarr_get(JVal *v, int i) {
    if (!v||v->type!=JArr||i<0||i>=v->arr.count) return NULL;
    return v->arr.items[i];
}

/* ── zip reader with deflate (zlib) ─────────────────────────────── */
#include <zlib.h>
static char *zip_read_file(const char *path, const char *name, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *data = malloc(sz);
    fread(data,1,sz,f); fclose(f);

    int pos=0;
    while (pos+30 < sz) {
        if (data[pos]!=0x50||data[pos+1]!=0x4b||data[pos+2]!=0x03||data[pos+3]!=0x04) break;
        int comp = data[pos+8] |(data[pos+9]<<8);
        int csz  = data[pos+18]|(data[pos+19]<<8)|(data[pos+20]<<16)|(data[pos+21]<<24);
        int usz  = data[pos+22]|(data[pos+23]<<8)|(data[pos+24]<<16)|(data[pos+25]<<24);
        int nlen = data[pos+26]|(data[pos+27]<<8);
        int elen = data[pos+28]|(data[pos+29]<<8);
        char fname[512]={0};
        int fnlen = nlen < 511 ? nlen : 511;
        memcpy(fname, data+pos+30, fnlen);
        int data_start = pos+30+nlen+elen;

        if (strcmp(fname, name)==0) {
            char *out = malloc(usz+1);
            if (comp==0) {
                memcpy(out, data+data_start, usz);
            } else if (comp==8) {
                uLongf destlen = usz;
                z_stream zs = {0};
                zs.next_in   = data+data_start;
                zs.avail_in  = csz;
                zs.next_out  = (Bytef*)out;
                zs.avail_out = usz;
                inflateInit2(&zs, -15); /* raw deflate */
                inflate(&zs, Z_FINISH);
                inflateEnd(&zs);
                (void)destlen;
            } else {
                free(out); free(data); return NULL;
            }
            out[usz]=0;
            if(out_len)*out_len=usz;
            free(data);
            return out;
        }
        pos = data_start + csz;
    }
    free(data);
    return NULL;
}

/* ── output buffer ──────────────────────────────────────────────── */
typedef struct { char *buf; int len; int cap; } Buf;
static void buf_init(Buf *b){b->buf=malloc(4096);b->cap=4096;b->len=0;b->buf[0]=0;}
static void buf_ensure(Buf *b,int n){while(b->len+n+1>=b->cap){b->cap*=2;b->buf=realloc(b->buf,b->cap);}}
static void buf_cat(Buf *b,const char *s){int n=strlen(s);buf_ensure(b,n);memcpy(b->buf+b->len,s,n);b->len+=n;b->buf[b->len]=0;}
static void buf_printf(Buf *b,const char *fmt,...){char tmp[2048];va_list ap;va_start(ap,fmt);vsnprintf(tmp,sizeof(tmp),fmt,ap);va_end(ap);buf_cat(b,tmp);}
static void buf_indent(Buf *b,int d){for(int i=0;i<d;i++)buf_cat(b,"    ");}

/* ── string helpers ──────────────────────────────────────────────── */
static void to_lowercase(const char *src, char *dst, int max_len) {
    int i;
    for (i=0; src[i]&&i<max_len-1; i++) {
        char c = src[i];
        dst[i] = (c>='A'&&c<='Z') ? (c-'A'+'a') : c;
    }
    dst[i]='\0';  /* terminate at actual end of copied string */
}

/* ── block decompiler ───────────────────────────────────────────── */

/* forward decl */
static void decompile_block(Buf *out, JVal *blocks, const char *uid, int depth);
static void decompile_input_expr(Buf *out, JVal *blocks, JVal *input_arr);

/* emit an expression from an input slot */
static void decompile_input_expr(Buf *out, JVal *blocks, JVal *input_arr) {
    /* input_arr is [type, value] or [type, block_uid, fallback] */
    if (!input_arr || input_arr->type != JArr) { buf_cat(out,"0"); return; }

    JVal *val = jarr_get(input_arr, 1);
    if (!val) { buf_cat(out,"0"); return; }

    /* if val is a string → it's a block uid */
    if (val->type == JStr) {
        const char *uid = val->string;
        JVal *block = jobj_get(blocks, uid);
        if (!block) { buf_printf(out,"(%s)", uid); return; }
        const char *op = jstr(jobj_get(block,"opcode"));
        JVal *inputs = jobj_get(block,"inputs");
        JVal *fields = jobj_get(block,"fields");
        if (!inputs) inputs = jval_new(JObj);

        /* reporter blocks - handle the most common ones */
        if (strcmp(op,"operator_add")==0)      {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM1"));
            buf_cat(out," + ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_subtract")==0) {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM1"));
            buf_cat(out," - ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_multiply")==0) {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM1"));
            buf_cat(out," * ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_divide")==0)   {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM1"));
            buf_cat(out," / ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_mod")==0)      {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM1"));
            buf_cat(out," mod ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_equals")==0)   {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND1"));
            buf_cat(out," = ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_gt")==0)       {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND1"));
            buf_cat(out," > ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_lt")==0)       {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND1"));
            buf_cat(out," < ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_and")==0)      {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND1"));
            buf_cat(out," and ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_or")==0)       {
            buf_cat(out,"(");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND1"));
            buf_cat(out," or ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_not")==0)      {
            buf_cat(out,"not (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"OPERAND"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_random")==0)   {
            buf_cat(out,"pick random ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"FROM"));
            buf_cat(out," to ");
            decompile_input_expr(out,blocks,jobj_get(inputs,"TO"));
            return;
        }
        if (strcmp(op,"operator_join")==0)     {
            buf_cat(out,"join (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"STRING1"));
            buf_cat(out,") (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"STRING2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_letter_of")==0){
            buf_cat(out,"letter (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"LETTER"));
            buf_cat(out,") of (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"STRING"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_length")==0)   {
            buf_cat(out,"length of (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"STRING"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_contains")==0) {
            decompile_input_expr(out,blocks,jobj_get(inputs,"STRING1"));
            buf_cat(out," contains (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"STRING2"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_round")==0)    {
            buf_cat(out,"round (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"operator_mathop")==0)   {
            JVal *opf = jobj_get(fields,"OPERATOR");
            const char *scratch_op = jstr(jarr_get(opf,0));
            const char *jappl_op = scratch_op;
            if (strcmp(scratch_op,"e ^")==0)    jappl_op = "exp";
            else if (strcmp(scratch_op,"10 ^")==0) jappl_op = "exp10";
            else if (strcmp(scratch_op,"ln")==0)   jappl_op = "ln";
            else if (strcmp(scratch_op,"log")==0)  jappl_op = "log";
            buf_printf(out,"%s (", jappl_op);
            decompile_input_expr(out,blocks,jobj_get(inputs,"NUM"));
            buf_cat(out,")");
            return;
        }
        /* sensing reporters */
        if (strcmp(op,"motion_xposition")==0)  { buf_cat(out,"x position"); return; }
        if (strcmp(op,"motion_yposition")==0)  { buf_cat(out,"y position"); return; }
        if (strcmp(op,"sensing_mousex")==0)    { buf_cat(out,"mouse x"); return; }
        if (strcmp(op,"sensing_mousey")==0)    { buf_cat(out,"mouse y"); return; }
        if (strcmp(op,"motion_direction")==0)  { buf_cat(out,"direction"); return; }
        if (strcmp(op,"looks_size")==0)        { buf_cat(out,"size"); return; }
        if (strcmp(op,"looks_costumenumbername")==0) {
            JVal *nf=jobj_get(fields,"NUMBER_NAME");
            const char *which=jstr(jarr_get(nf,0));
            if(strcmp(which,"number")==0) buf_cat(out,"costume number");
            else buf_cat(out,"costume name");
            return;
        }
        if (strcmp(op,"looks_backdropnumbername")==0) {
            JVal *nf=jobj_get(fields,"NUMBER_NAME");
            const char *which=jstr(jarr_get(nf,0));
            if(strcmp(which,"number")==0) buf_cat(out,"backdrop number");
            else buf_cat(out,"backdrop name");
            return;
        }
        if (strcmp(op,"sound_volume")==0)      { buf_cat(out,"volume"); return; }
        if (strcmp(op,"sensing_answer")==0)    { buf_cat(out,"answer"); return; }
        if (strcmp(op,"sensing_timer")==0)     { buf_cat(out,"timer"); return; }
        if (strcmp(op,"sensing_mousedown")==0) { buf_cat(out,"mouse down"); return; }
        if (strcmp(op,"sensing_loudness")==0)  { buf_cat(out,"loudness"); return; }
        if (strcmp(op,"sensing_username")==0)  { buf_cat(out,"username"); return; }
        if (strcmp(op,"sensing_online")==0)    { buf_cat(out,"online"); return; }
        if (strcmp(op,"sensing_dayssince2000")==0) { buf_cat(out,"days since 2000"); return; }
        if (strcmp(op,"sensing_touchingcolor")==0) {
            JVal *ci=jobj_get(inputs,"COLOR");
            JVal *cv=ci?jarr_get(ci,1):NULL;
            const char *col=(cv&&cv->type==JArr)?jstr(jarr_get(cv,1)):"#000000";
            buf_printf(out,"touching color (%s)",col); return;
        }
        if (strcmp(op,"sensing_coloristouchingcolor")==0) {
            JVal *ci=jobj_get(inputs,"COLOR");  JVal *cv=ci?jarr_get(ci,1):NULL;
            JVal *ci2=jobj_get(inputs,"COLOR2"); JVal *cv2=ci2?jarr_get(ci2,1):NULL;
            const char *c1=(cv&&cv->type==JArr)?jstr(jarr_get(cv,1)):"#000000";
            const char *c2=(cv2&&cv2->type==JArr)?jstr(jarr_get(cv2,1)):"#000000";
            buf_printf(out,"color (%s) touching (%s)",c1,c2); return;
        }
        if (strcmp(op,"sensing_current")==0) {
            JVal *mf=jobj_get(fields,"CURRENTMENU");
            const char *which=jstr(jarr_get(mf,0));
            const char *jappl_which=
                strcmp(which,"YEAR")==0      ? "year"        :
                strcmp(which,"MONTH")==0     ? "month"       :
                strcmp(which,"DATE")==0      ? "date"        :
                strcmp(which,"DAYOFWEEK")==0 ? "day of week" :
                strcmp(which,"HOUR")==0      ? "hour"        :
                strcmp(which,"MINUTE")==0    ? "minute"      : "second";
            buf_printf(out,"current %s",jappl_which); return;
        }
        if (strcmp(op,"sensing_of")==0) {
            JVal *pf=jobj_get(fields,"PROPERTY");
            JVal *oi=jobj_get(inputs,"OBJECT"); JVal *ouid=oi?jarr_get(oi,1):NULL;
            const char *menu_uid=(ouid&&ouid->type==JStr)?ouid->string:"";
            JVal *mb=jobj_get(blocks,menu_uid);
            JVal *mf=mb?jobj_get(jobj_get(mb,"fields"),"OBJECT"):NULL;
            const char *sprite=jstr(jarr_get(mf,0));
            if (strcmp(sprite, "_stage_") == 0) {
                buf_printf(out,"sensing (_stage_) (%s)", jstr(jarr_get(pf,0)));
            } else {
                buf_printf(out,"sensing (%s) (%s)", sprite, jstr(jarr_get(pf,0)));
            }
            return;
        }
        if (strcmp(op,"sensing_keypressed")==0){
            JVal *ki=jobj_get(inputs,"KEY_OPTION"); JVal *kuid=ki?jarr_get(ki,1):NULL;
            const char *kuid_s=(kuid&&kuid->type==JStr)?kuid->string:"";
            JVal *kb=jobj_get(blocks,kuid_s);
            JVal *kf=kb?jobj_get(jobj_get(kb,"fields"),"KEY_OPTION"):NULL;
            const char *key = jstr(jarr_get(kf,0));
            char key_clean[64]; strncpy(key_clean, key, 63); key_clean[63]=0;
            char *sp = strchr(key_clean, ' ');
            if (sp) *sp = '\0';
            buf_printf(out,"key (%s) pressed", key_clean);
            return;
        }
        if (strcmp(op,"sensing_touchingobject")==0){
            JVal *ti=jobj_get(inputs,"TOUCHINGOBJECTMENU"); JVal *tuid=ti?jarr_get(ti,1):NULL;
            const char *tuid_s=(tuid&&tuid->type==JStr)?tuid->string:"";
            JVal *tb=jobj_get(blocks,tuid_s);
            JVal *tf=tb?jobj_get(jobj_get(tb,"fields"),"TOUCHINGOBJECTMENU"):NULL;
            const char *t=jstr(jarr_get(tf,0));
            if(strcmp(t,"_mouse_")==0) buf_cat(out,"touching (mouse pointer)");
            else if(strcmp(t,"_edge_")==0) buf_cat(out,"touching (edge)");
            else buf_printf(out,"touching (%s)",t);
            return;
        }
        if (strcmp(op,"sensing_distanceto")==0){
            JVal *di=jobj_get(inputs,"DISTANCETOMENU"); JVal *duid=di?jarr_get(di,1):NULL;
            const char *duid_s=(duid&&duid->type==JStr)?duid->string:"";
            JVal *db=jobj_get(blocks,duid_s);
            JVal *df=db?jobj_get(jobj_get(db,"fields"),"DISTANCETOMENU"):NULL;
            const char *t=jstr(jarr_get(df,0));
            if(strcmp(t,"_mouse_")==0) buf_cat(out,"distance to (mouse pointer)");
            else buf_printf(out,"distance to (%s)",t);
            return;
        }
        /* variable reporter */
        if (strcmp(op,"data_variable")==0){
            JVal *vf=jobj_get(fields,"VARIABLE");
            buf_printf(out,"[%s]",jstr(jarr_get(vf,0)));
            return;
        }
        if (strcmp(op,"data_itemoflist")==0){
            JVal *lf=jobj_get(fields,"LIST");
            buf_cat(out,"item (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"INDEX"));
            buf_printf(out,") of <%s>",jstr(jarr_get(lf,0)));
            return;
        }
        if (strcmp(op,"data_lengthoflist")==0){
            JVal *lf=jobj_get(fields,"LIST");
            buf_printf(out,"length of <%s>",jstr(jarr_get(lf,0)));
            return;
        }
        if (strcmp(op,"data_listcontainsitem")==0){
            JVal *lf=jobj_get(fields,"LIST");
            buf_printf(out,"<%s> contains (",jstr(jarr_get(lf,0)));
            decompile_input_expr(out,blocks,jobj_get(inputs,"ITEM"));
            buf_cat(out,")");
            return;
        }
        if (strcmp(op,"data_itemnumoflist")==0){
            JVal *lf=jobj_get(fields,"LIST");
            buf_cat(out,"item num (");
            decompile_input_expr(out,blocks,jobj_get(inputs,"ITEM"));
            buf_printf(out,") of <%s>",jstr(jarr_get(lf,0)));
            return;
        }
        /* argument reporter */
        if (strcmp(op,"argument_reporter_string_number")==0||
            strcmp(op,"argument_reporter_boolean")==0){
            JVal *vf=jobj_get(fields,"VALUE");
            buf_printf(out,"(%s)",jstr(jarr_get(vf,0)));
            return;
        }
        /* unknown reporter — emit as number 0 to keep valid syntax */
        buf_cat(out,"0");
        return;
    }

    /* val is an array → primitive [type, value] */
    if (val->type == JArr) {
        JVal *pval = jarr_get(val, 1);
        if (!pval) { buf_cat(out,"0"); return; }
        if (pval->type==JStr)  {
            buf_printf(out,"(%s)", pval->string);
            return;
        }
        if (pval->type==JNum)  {
            double d = pval->number;
            if (d == (int)d) buf_printf(out,"%d", (int)d);
            else buf_printf(out,"%g", d);
            return;
        }
        buf_cat(out,"0");
        return;
    }

    buf_cat(out,"0");
}

/* helper: emit input wrapped in () */
static void emit_input(Buf *out, JVal *blocks, JVal *inputs, const char *name) {
    JVal *slot = jobj_get(inputs, name);
    if (!slot) { buf_cat(out,"0"); return; }
    decompile_input_expr(out, blocks, slot);
}

/* decompile a chain of statement blocks starting at uid */
static void decompile_chain(Buf *out, JVal *blocks, const char *uid, int depth) {
    while (uid && uid[0]) {
        decompile_block(out, blocks, uid, depth);
        JVal *block = jobj_get(blocks, uid);
        if (!block) break;
        JVal *nxt = jobj_get(block,"next");
        uid = (nxt && nxt->type==JStr) ? nxt->string : NULL;
    }
}

static void decompile_block(Buf *out, JVal *blocks, const char *uid, int depth) {
    JVal *block = jobj_get(blocks, uid);
    if (!block) return;

    const char *op = jstr(jobj_get(block,"opcode"));
    JVal *inputs   = jobj_get(block,"inputs");
    JVal *fields   = jobj_get(block,"fields");
    if (!inputs) inputs = jval_new(JObj);
    if (!fields) fields = jval_new(JObj);

    buf_indent(out, depth);

    /* ── motion ── */
    if (strcmp(op,"motion_movesteps")==0)      { buf_cat(out,"move "); emit_input(out,blocks,inputs,"STEPS"); buf_cat(out," steps\n"); return; }
    if (strcmp(op,"motion_turnright")==0)      { buf_cat(out,"turn right "); emit_input(out,blocks,inputs,"DEGREES"); buf_cat(out," degrees\n"); return; }
    if (strcmp(op,"motion_turnleft")==0)       { buf_cat(out,"turn left "); emit_input(out,blocks,inputs,"DEGREES"); buf_cat(out," degrees\n"); return; }
    if (strcmp(op,"motion_gotoxy")==0)         { buf_cat(out,"go to "); emit_input(out,blocks,inputs,"X"); buf_cat(out," "); emit_input(out,blocks,inputs,"Y"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"motion_goto")==0) {
        JVal *menu_slot = jobj_get(inputs,"TO");
        JVal *menu_uid_v = menu_slot ? jarr_get(menu_slot,1) : NULL;
        const char *menu_uid = (menu_uid_v&&menu_uid_v->type==JStr)?menu_uid_v->string:"";
        JVal *menu_block = jobj_get(blocks,menu_uid);
        JVal *minp = menu_block ? jobj_get(jobj_get(menu_block,"inputs"),"TO") : NULL;
        JVal *mf   = menu_block ? jobj_get(jobj_get(menu_block,"fields"),"TO")  : NULL;
        const char *tgt = minp ? jstr(jarr_get(minp,0)) : jstr(jarr_get(mf,0));
        if (strcmp(tgt,"_random_")==0) buf_cat(out,"go to (random position)\n");
        else if (strcmp(tgt,"_mouse_")==0) buf_cat(out,"go to (mouse pointer)\n");
        else buf_printf(out,"go to (%s)\n",tgt);
        return;
    }
    if (strcmp(op,"motion_glidesecstoxy")==0)  { buf_cat(out,"glide "); emit_input(out,blocks,inputs,"SECS"); buf_cat(out," secs to "); emit_input(out,blocks,inputs,"X"); buf_cat(out," "); emit_input(out,blocks,inputs,"Y"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"motion_glideto")==0) {
        JVal *menu_slot = jobj_get(inputs,"TO");
        JVal *menu_uid_v = menu_slot ? jarr_get(menu_slot,1) : NULL;
        const char *menu_uid = (menu_uid_v&&menu_uid_v->type==JStr)?menu_uid_v->string:"";
        JVal *menu_block = jobj_get(blocks,menu_uid);
        JVal *minp = menu_block ? jobj_get(jobj_get(menu_block,"inputs"),"TO") : NULL;
        JVal *mf   = menu_block ? jobj_get(jobj_get(menu_block,"fields"),"TO")  : NULL;
        const char *tgt = minp ? jstr(jarr_get(minp,0)) : jstr(jarr_get(mf,0));
        buf_cat(out,"glide "); emit_input(out,blocks,inputs,"SECS"); buf_cat(out," secs to ");
        if (strcmp(tgt,"_random_")==0) buf_cat(out,"(random position)");
        else if (strcmp(tgt,"_mouse_")==0) buf_cat(out,"(mouse pointer)");
        else buf_printf(out,"(%s)",tgt);
        buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"motion_setx")==0)           { buf_cat(out,"set x to "); emit_input(out,blocks,inputs,"X"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"motion_sety")==0)           { buf_cat(out,"set y to "); emit_input(out,blocks,inputs,"Y"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"motion_changexby")==0)      { buf_cat(out,"change x by "); emit_input(out,blocks,inputs,"DX"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"motion_changeyby")==0)      { buf_cat(out,"change y by "); emit_input(out,blocks,inputs,"DY"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"motion_pointindirection")==0){ buf_cat(out,"point in direction "); emit_input(out,blocks,inputs,"DIRECTION"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"motion_pointtowards")==0) {
        JVal *ms=jobj_get(inputs,"TOWARDS"); JVal *mu=ms?jarr_get(ms,1):NULL;
        const char *muid=(mu&&mu->type==JStr)?mu->string:"";
        JVal *mb=jobj_get(blocks,muid);
        JVal *minp=mb?jobj_get(jobj_get(mb,"inputs"),"TOWARDS"):NULL;
        JVal *mf=mb?jobj_get(jobj_get(mb,"fields"),"TOWARDS"):NULL;
        const char *t=minp?jstr(jarr_get(minp,0)):jstr(jarr_get(mf,0));
        if(strcmp(t,"_mouse_")==0) buf_cat(out,"point towards (mouse pointer)\n");
        else buf_printf(out,"point towards (%s)\n",t);
        return;
    }

    /* ── looks ── */
    if (strcmp(op,"looks_say")==0)             { buf_cat(out,"say "); emit_input(out,blocks,inputs,"MESSAGE"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"looks_sayforsecs")==0)      { buf_cat(out,"say "); emit_input(out,blocks,inputs,"MESSAGE"); buf_cat(out," for "); emit_input(out,blocks,inputs,"SECS"); buf_cat(out," seconds\n"); return; }
    if (strcmp(op,"looks_think")==0)           { buf_cat(out,"think "); emit_input(out,blocks,inputs,"MESSAGE"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"looks_thinkforsecs")==0)    { buf_cat(out,"think "); emit_input(out,blocks,inputs,"MESSAGE"); buf_cat(out," for "); emit_input(out,blocks,inputs,"SECS"); buf_cat(out," seconds\n"); return; }
    if (strcmp(op,"looks_show")==0)            { buf_cat(out,"show\n"); return; }
    if (strcmp(op,"looks_hide")==0)            { buf_cat(out,"hide\n"); return; }
    if (strcmp(op,"looks_nextcostume")==0)     { buf_cat(out,"next costume\n"); return; }
    if (strcmp(op,"looks_setsizeto")==0)       { buf_cat(out,"set size to "); emit_input(out,blocks,inputs,"SIZE"); buf_cat(out," %\n"); return; }
    if (strcmp(op,"looks_changesizeby")==0)    { buf_cat(out,"change size by "); emit_input(out,blocks,inputs,"CHANGE"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"looks_switchcostumeto")==0) {
        JVal *ms=jobj_get(inputs,"COSTUME"); JVal *mu=ms?jarr_get(ms,1):NULL;
        const char *muid=(mu&&mu->type==JStr)?mu->string:"";
        JVal *mb=jobj_get(blocks,muid);
        JVal *minp=mb?jobj_get(jobj_get(mb,"inputs"),"COSTUME"):NULL;
        JVal *mf=mb?jobj_get(jobj_get(mb,"fields"),"COSTUME"):NULL;
        buf_printf(out,"switch costume to (%s)\n",minp?jstr(jarr_get(minp,0)):jstr(jarr_get(mf,0))); return;
    }
    if (strcmp(op,"looks_switchbackdropto")==0){
        JVal *ms=jobj_get(inputs,"BACKDROP"); JVal *mu=ms?jarr_get(ms,1):NULL;
        const char *muid=(mu&&mu->type==JStr)?mu->string:"";
        JVal *mb=jobj_get(blocks,muid);
        JVal *minp=mb?jobj_get(jobj_get(mb,"inputs"),"BACKDROP"):NULL;
        JVal *mf=mb?jobj_get(jobj_get(mb,"fields"),"BACKDROP"):NULL;
        buf_printf(out,"switch backdrop to (%s)\n",minp?jstr(jarr_get(minp,0)):jstr(jarr_get(mf,0))); return;
    }

    /* ── sound ── */
    if (strcmp(op,"sound_play")==0||strcmp(op,"sound_playuntildone")==0) {
        JVal *ms=jobj_get(inputs,"SOUND_MENU"); JVal *mu=ms?jarr_get(ms,1):NULL;
        const char *muid=(mu&&mu->type==JStr)?mu->string:"";
        JVal *mb=jobj_get(blocks,muid);
        JVal *minp=mb?jobj_get(jobj_get(mb,"inputs"),"SOUND_MENU"):NULL;
        JVal *mf=mb?jobj_get(jobj_get(mb,"fields"),"SOUND_MENU"):NULL;
        buf_printf(out,"play sound (%s)%s\n",minp?jstr(jarr_get(minp,0)):jstr(jarr_get(mf,0)),
            strcmp(op,"sound_playuntildone")==0?" until done":"");
        return;
    }
    if (strcmp(op,"sound_stopallsounds")==0)   { buf_cat(out,"stop all sounds\n"); return; }
    if (strcmp(op,"sound_setvolumeto")==0)     { buf_cat(out,"set volume to "); emit_input(out,blocks,inputs,"VOLUME"); buf_cat(out," %\n"); return; }
    if (strcmp(op,"sound_changevolumeby")==0)  { buf_cat(out,"change volume by "); emit_input(out,blocks,inputs,"VOLUME"); buf_cat(out,"\n"); return; }

    /* ── events ── */
    if (strcmp(op,"event_broadcast")==0||strcmp(op,"event_broadcastandwait")==0) {
        JVal *ms=jobj_get(inputs,"BROADCAST_INPUT"); JVal *mu=ms?jarr_get(ms,1):NULL;
        const char *muid=(mu&&mu->type==JStr)?mu->string:"";
        JVal *mb=jobj_get(blocks,muid);
        JVal *minp=mb?jobj_get(jobj_get(mb,"inputs"),"BROADCAST_OPTION"):NULL;
        JVal *mf=mb?jobj_get(jobj_get(mb,"fields"),"BROADCAST_OPTION"):NULL;
        buf_printf(out,"broadcast (%s)%s\n",minp?jstr(jarr_get(minp,0)):jstr(jarr_get(mf,0)),
            strcmp(op,"event_broadcastandwait")==0?" and wait":"");
        return;
    }

    /* ── control ── */
    if (strcmp(op,"control_wait")==0)          { buf_cat(out,"wait "); emit_input(out,blocks,inputs,"DURATION"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"control_wait_until")==0)    { buf_cat(out,"wait until "); emit_input(out,blocks,inputs,"CONDITION"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"control_forever")==0) {
        buf_cat(out,"forever\n"); buf_indent(out,depth); buf_cat(out,"{\n");
        JVal *sub=jobj_get(inputs,"SUBSTACK");
        JVal *subuid=sub?jarr_get(sub,1):NULL;
        if(subuid&&subuid->type==JStr) decompile_chain(out,blocks,subuid->string,depth+1);
        buf_indent(out,depth); buf_cat(out,"}\n"); return;
    }
    if (strcmp(op,"control_repeat")==0) {
        buf_cat(out,"repeat "); emit_input(out,blocks,inputs,"TIMES"); buf_cat(out,"\n");
        buf_indent(out,depth); buf_cat(out,"{\n");
        JVal *sub=jobj_get(inputs,"SUBSTACK"); JVal *suid=sub?jarr_get(sub,1):NULL;
        if(suid&&suid->type==JStr) decompile_chain(out,blocks,suid->string,depth+1);
        buf_indent(out,depth); buf_cat(out,"}\n"); return;
    }
    if (strcmp(op,"control_if")==0) {
        buf_cat(out,"if "); emit_input(out,blocks,inputs,"CONDITION"); buf_cat(out,"\n");
        buf_indent(out,depth); buf_cat(out,"{\n");
        JVal *sub=jobj_get(inputs,"SUBSTACK"); JVal *suid=sub?jarr_get(sub,1):NULL;
        if(suid&&suid->type==JStr) decompile_chain(out,blocks,suid->string,depth+1);
        buf_indent(out,depth); buf_cat(out,"}\n"); return;
    }
    if (strcmp(op,"control_if_else")==0) {
        buf_cat(out,"if "); emit_input(out,blocks,inputs,"CONDITION"); buf_cat(out,"\n");
        buf_indent(out,depth); buf_cat(out,"{\n");
        JVal *sub=jobj_get(inputs,"SUBSTACK"); JVal *suid=sub?jarr_get(sub,1):NULL;
        if(suid&&suid->type==JStr) decompile_chain(out,blocks,suid->string,depth+1);
        buf_indent(out,depth); buf_cat(out,"}\n");
        buf_indent(out,depth); buf_cat(out,"else\n");
        buf_indent(out,depth); buf_cat(out,"{\n");
        JVal *sub2=jobj_get(inputs,"SUBSTACK2"); JVal *s2uid=sub2?jarr_get(sub2,1):NULL;
        if(s2uid&&s2uid->type==JStr) decompile_chain(out,blocks,s2uid->string,depth+1);
        buf_indent(out,depth); buf_cat(out,"}\n"); return;
    }
    if (strcmp(op,"control_repeat_until")==0) {
        buf_cat(out,"repeat until "); emit_input(out,blocks,inputs,"CONDITION"); buf_cat(out,"\n");
        buf_indent(out,depth); buf_cat(out,"{\n");
        JVal *sub=jobj_get(inputs,"SUBSTACK"); JVal *suid=sub?jarr_get(sub,1):NULL;
        if(suid&&suid->type==JStr) decompile_chain(out,blocks,suid->string,depth+1);
        buf_indent(out,depth); buf_cat(out,"}\n"); return;
    }
    if (strcmp(op,"control_stop")==0) {
        JVal *sf=jobj_get(fields,"STOP_OPTION");
        buf_printf(out,"stop %s\n",jstr(jarr_get(sf,0))); return;
    }

    /* ── sensing ── */
    if (strcmp(op,"sensing_askandwait")==0)    { buf_cat(out,"ask "); emit_input(out,blocks,inputs,"QUESTION"); buf_cat(out,"\n"); return; }
    if (strcmp(op,"sensing_resettimer")==0)    { buf_cat(out,"reset timer\n"); return; }

    /* ── variables ── */
    if (strcmp(op,"data_setvariableto")==0) {
        JVal *vf=jobj_get(fields,"VARIABLE");
        buf_printf(out,"set [%s] to ",jstr(jarr_get(vf,0)));
        emit_input(out,blocks,inputs,"VALUE"); buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"data_changevariableby")==0) {
        JVal *vf=jobj_get(fields,"VARIABLE");
        buf_printf(out,"change [%s] by ",jstr(jarr_get(vf,0)));
        emit_input(out,blocks,inputs,"VALUE"); buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"data_showvariable")==0) { JVal *vf=jobj_get(fields,"VARIABLE"); buf_printf(out,"show variable [%s]\n",jstr(jarr_get(vf,0))); return; }
    if (strcmp(op,"data_hidevariable")==0) { JVal *vf=jobj_get(fields,"VARIABLE"); buf_printf(out,"hide variable [%s]\n",jstr(jarr_get(vf,0))); return; }

    /* ── lists ── */
    if (strcmp(op,"data_addtolist")==0) {
        JVal *lf=jobj_get(fields,"LIST");
        buf_cat(out,"add "); emit_input(out,blocks,inputs,"ITEM");
        buf_printf(out," to <%s>\n",jstr(jarr_get(lf,0))); return;
    }
    if (strcmp(op,"data_deleteoflist")==0) {
        JVal *lf=jobj_get(fields,"LIST");
        buf_cat(out,"delete "); emit_input(out,blocks,inputs,"INDEX");
        buf_printf(out," of <%s>\n",jstr(jarr_get(lf,0))); return;
    }
    if (strcmp(op,"data_deletealloflist")==0)  { JVal *lf=jobj_get(fields,"LIST"); buf_printf(out,"delete all of <%s>\n",jstr(jarr_get(lf,0))); return; }
    if (strcmp(op,"data_insertatlist")==0) {
        JVal *lf=jobj_get(fields,"LIST");
        buf_cat(out,"insert "); emit_input(out,blocks,inputs,"ITEM");
        buf_cat(out," at "); emit_input(out,blocks,inputs,"INDEX");
        buf_printf(out," of <%s>\n",jstr(jarr_get(lf,0))); return;
    }
    if (strcmp(op,"data_replaceitemoflist")==0) {
        JVal *lf=jobj_get(fields,"LIST");
        buf_cat(out,"replace item "); emit_input(out,blocks,inputs,"INDEX");
        buf_printf(out," of <%s> with ",jstr(jarr_get(lf,0)));
        emit_input(out,blocks,inputs,"ITEM"); buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"data_showlist")==0) { JVal *lf=jobj_get(fields,"LIST"); buf_printf(out,"show list <%s>\n",jstr(jarr_get(lf,0))); return; }
    if (strcmp(op,"data_hidelist")==0) { JVal *lf=jobj_get(fields,"LIST"); buf_printf(out,"hide list <%s>\n",jstr(jarr_get(lf,0))); return; }

    /* ── custom blocks ── */
    if (strcmp(op,"procedures_call")==0) {
        JVal *mut = jobj_get(block,"mutation");
        const char *proccode = jstr(jobj_get(mut,"proccode"));
        char name_buf[256]; strncpy(name_buf, proccode, sizeof(name_buf)-1); name_buf[255]=0;
        char *pct = strstr(name_buf," %s");
        if (pct) *pct = '\0';
        buf_printf(out,"%s", name_buf);
        const char *arg_ids_str = jstr(jobj_get(mut,"argumentids"));
        const char *p2 = arg_ids_str;
        while (*p2) {
            while (*p2 && *p2 != '"') p2++;
            if (!*p2) break;
            p2++;
            const char *start = p2;
            while (*p2 && *p2 != '"') p2++;
            if (*p2 == '"') {
                char auid[64]; int alen = p2-start < 63 ? p2-start : 63;
                strncpy(auid, start, alen); auid[alen]=0;
                p2++;
                JVal *ainp = jobj_get(inputs, auid);
                if (ainp) { buf_cat(out," "); decompile_input_expr(out,blocks,ainp); }
            }
        }
        buf_cat(out,"\n"); return;
    }

    /* ── motion extras ── */
    if (strcmp(op,"motion_ifonedgebounce")==0) { buf_cat(out,"if on edge bounce\n"); return; }
    if (strcmp(op,"motion_setrotationstyle")==0) {
        JVal *sf=jobj_get(fields,"STYLE");
        buf_printf(out,"set rotation style to (%s)\n",jstr(jarr_get(sf,0))); return;
    }

    /* ── looks extras ── */
    if (strcmp(op,"looks_nextbackdrop")==0) { buf_cat(out,"next backdrop\n"); return; }
    if (strcmp(op,"looks_gotofrontback")==0) {
        JVal *ff=jobj_get(fields,"FRONT_BACK");
        const char *dir=jstr(jarr_get(ff,0));
        char dir_lower[16]; to_lowercase(dir,dir_lower,16);
        buf_printf(out,"go to %s layer\n",dir_lower); return;
    }
    if (strcmp(op,"looks_goforwardbackwardlayers")==0) {
        JVal *df=jobj_get(fields,"FORWARD_BACKWARD");
        const char *dir_upper=jstr(jarr_get(df,0));
        char dir_lower[16]; to_lowercase(dir_upper,dir_lower,16);
        buf_printf(out,"go %s ",dir_lower);
        emit_input(out,blocks,inputs,"NUM");
        buf_cat(out," layers\n"); return;
    }
    if (strcmp(op,"looks_changeeffectby")==0) {
        JVal *ef=jobj_get(fields,"EFFECT");
        const char *effect_upper=jstr(jarr_get(ef,0));
        char effect_lower[32]; to_lowercase(effect_upper,effect_lower,32);
        buf_printf(out,"change %s effect by ",effect_lower);
        emit_input(out,blocks,inputs,"CHANGE"); buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"looks_seteffectto")==0) {
        JVal *ef=jobj_get(fields,"EFFECT");
        const char *effect_upper=jstr(jarr_get(ef,0));
        char effect_lower[32]; to_lowercase(effect_upper,effect_lower,32);
        buf_printf(out,"set %s effect to ",effect_lower);
        emit_input(out,blocks,inputs,"VALUE"); buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"looks_cleargraphiceffects")==0) { buf_cat(out,"clear graphic effects\n"); return; }

    /* ── sound extras ── */
    if (strcmp(op,"sound_changeeffectby")==0) {
        JVal *ef=jobj_get(fields,"EFFECT");
        const char *ename=jstr(jarr_get(ef,0));
        char elower[32]; to_lowercase(ename,elower,32);
        buf_printf(out,"change %s effect by ", elower);
        emit_input(out,blocks,inputs,"VALUE"); buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"sound_seteffectto")==0) {
        JVal *ef=jobj_get(fields,"EFFECT");
        const char *ename=jstr(jarr_get(ef,0));
        char elower[32]; to_lowercase(ename,elower,32);
        buf_printf(out,"set %s effect to ", elower);
        emit_input(out,blocks,inputs,"VALUE"); buf_cat(out,"\n"); return;
    }
    if (strcmp(op,"sound_cleareffects")==0) { buf_cat(out,"clear sound effects\n"); return; }

    /* ── sensing statements ── */
    if (strcmp(op,"sensing_setdragmode")==0) {
        JVal *df=jobj_get(fields,"DRAG_MODE");
        buf_printf(out,"set drag mode to (%s)\n",jstr(jarr_get(df,0))); return;
    }

    /* ── clone control ── */
    if (strcmp(op,"control_create_clone_of")==0) {
        JVal *ci=jobj_get(inputs,"CLONE_OPTION"); JVal *cuid=ci?jarr_get(ci,1):NULL;
        const char *cuid_s=(cuid&&cuid->type==JStr)?cuid->string:"";
        JVal *cb=jobj_get(blocks,cuid_s);
        JVal *cf=cb?jobj_get(jobj_get(cb,"fields"),"CLONE_OPTION"):NULL;
        const char *tgt=jstr(jarr_get(cf,0));
        if(strcmp(tgt,"_myself_")==0) buf_cat(out,"create clone (myself)\n");
        else buf_printf(out,"create clone (%s)\n",tgt);
        return;
    }
    if (strcmp(op,"control_delete_this_clone")==0) { buf_cat(out,"delete this clone\n"); return; }

    /* ── unknown (emit as comment so file stays parseable) ── */
    buf_printf(out,"// TODO: %s\n", op);
}

/* ── hat block → when (...) ─────────────────────────────────────── */
static void decompile_hat(Buf *out, JVal *blocks, const char *uid, int depth) {
    JVal *block  = jobj_get(blocks, uid);
    if (!block) return;
    const char *op = jstr(jobj_get(block,"opcode"));
    JVal *inputs   = jobj_get(block,"inputs");
    JVal *fields   = jobj_get(block,"fields");
    if (!inputs) inputs = jval_new(JObj);
    if (!fields) fields = jval_new(JObj);

    buf_indent(out, depth);
    if (strcmp(op,"event_whenflagclicked")==0) {
        buf_cat(out,"when (green flag)\n");
    } else if (strcmp(op,"event_whenbroadcastreceived")==0) {
        JVal *bf=jobj_get(fields,"BROADCAST_OPTION");
        buf_printf(out,"when (message %s)\n",jstr(jarr_get(bf,0)));
    } else if (strcmp(op,"event_whenkeypressed")==0) {
        JVal *kf=jobj_get(fields,"KEY_OPTION");
        buf_printf(out,"when (key %s pressed)\n",jstr(jarr_get(kf,0)));
    } else if (strcmp(op,"event_whenthisspriteclicked")==0) {
        buf_cat(out,"when (this sprite clicked)\n");
    } else if (strcmp(op,"control_start_as_clone")==0) {
        buf_cat(out,"when (start as clone)\n");
    } else if (strcmp(op,"event_whenbackdropswitchesto")==0) {
        JVal *bf=jobj_get(fields,"BACKDROP");
        buf_printf(out,"when (backdrop switches to %s)\n",jstr(jarr_get(bf,0)));
    } else if (strcmp(op,"event_whengreaterthan")==0) {
        JVal *mf=jobj_get(fields,"WHENGREATERTHANMENU");
        const char *which=jstr(jarr_get(mf,0));
        const char *jappl_which=strcmp(which,"LOUDNESS")==0?"loudness":"timer";
        buf_printf(out,"when (%s > ) ",jappl_which);
        emit_input(out,blocks,inputs,"VALUE");
        buf_cat(out,"\n");
    } else if (strcmp(op,"procedures_definition")==0) {
        JVal *proto_inp = jobj_get(inputs,"custom_block");
        const char *proto_uid = jstr(jarr_get(proto_inp,1));
        JVal *proto = jobj_get(blocks,proto_uid);
        JVal *pmut  = proto ? jobj_get(proto,"mutation") : NULL;
        const char *proccode = pmut ? jstr(jobj_get(pmut,"proccode")) : "";
        char pname[256]; strncpy(pname,proccode,sizeof(pname)-1); pname[255]=0;
        char *pct=strstr(pname," %s"); if(pct)*pct='\0';
        buf_printf(out,"define (%s)\n", pname);
    } else {
        /* Is this a floating reporter block (no next, no hat)?
           If so, wrap it in a set statement so it round-trips as a visible canvas block.
           Otherwise fall back to an empty floating script. */
        JVal *nxt2 = jobj_get(block,"next");
        int has_next = (nxt2 && nxt2->type==JStr && nxt2->string[0]);
        /* Reporter opcodes: operator_*, sensing reporters, motion reporters, looks reporters,
           data reporters, argument reporters */
        int is_reporter = (!has_next && (
            strncmp(op,"operator_",9)==0 ||
            strcmp(op,"sensing_answer")==0 || strcmp(op,"sensing_timer")==0 ||
            strcmp(op,"sensing_mousedown")==0 || strcmp(op,"sensing_mousex")==0 ||
            strcmp(op,"sensing_mousey")==0 || strcmp(op,"sensing_loudness")==0 ||
            strcmp(op,"sensing_username")==0 || strcmp(op,"sensing_dayssince2000")==0 ||
            strcmp(op,"sensing_current")==0 || strcmp(op,"sensing_of")==0 ||
            strcmp(op,"sensing_keypressed")==0 || strcmp(op,"sensing_touchingobject")==0 ||
            strcmp(op,"sensing_touchingcolor")==0 || strcmp(op,"sensing_coloristouchingcolor")==0 ||
            strcmp(op,"sensing_distanceto")==0 ||
            strcmp(op,"motion_xposition")==0 || strcmp(op,"motion_yposition")==0 ||
            strcmp(op,"motion_direction")==0 ||
            strcmp(op,"looks_size")==0 || strcmp(op,"looks_costumenumbername")==0 ||
            strcmp(op,"looks_backdropnumbername")==0 || strcmp(op,"sound_volume")==0 ||
            strcmp(op,"data_variable")==0 || strcmp(op,"data_itemoflist")==0 ||
            strcmp(op,"data_lengthoflist")==0 || strcmp(op,"data_listcontainsitem")==0 ||
            strcmp(op,"data_itemnumoflist")==0 ||
            strcmp(op,"argument_reporter_string_number")==0 ||
            strcmp(op,"argument_reporter_boolean")==0
        ));
        if (is_reporter) {
            /* Floating reporter — emit a harmless placeholder so the emitter
               produces a visible top-level block on the Scratch canvas. */
            buf_indent(out, depth); buf_cat(out,"{\n");
            buf_indent(out, depth+1); buf_printf(out,"// was: %s\n", op);
            buf_indent(out, depth+1); buf_cat(out,"set [my variable] to 0\n");
            buf_indent(out, depth); buf_cat(out,"}\n\n");
        } else {
            buf_printf(out,"// floating: %s\n", op);
            buf_indent(out, depth); buf_cat(out,"{\n");
            /* include the block itself, not just its next */
            decompile_chain(out, blocks, uid, depth+1);
            buf_indent(out, depth); buf_cat(out,"}\n\n");
        }
        return;
    }

    buf_indent(out, depth); buf_cat(out,"{\n");
    JVal *nxt = jobj_get(block,"next");
    if (nxt && nxt->type==JStr && nxt->string[0])
        decompile_chain(out, blocks, nxt->string, depth+1);
    buf_indent(out, depth); buf_cat(out,"}\n\n");
}

/* ── target (sprite/stage) decompiler ───────────────────────────── */
static void decompile_target(Buf *out, JVal *target, int is_stage,
                              JVal *stage_target) {
    const char *name = jstr(jobj_get(target,"name"));
    JVal *blocks     = jobj_get(target,"blocks");
    JVal *variables  = jobj_get(target,"variables");
    JVal *lists      = jobj_get(target,"lists");
    if (!blocks) return;

    if (is_stage) {
        if (variables) {
            for (int i=0;i<variables->obj.count;i++) {
                JVal *varr = variables->obj.pairs[i].val;
                const char *vname = (varr&&varr->type==JArr&&varr->arr.count>0&&varr->arr.items[0]->type==JStr)
                    ? varr->arr.items[0]->string : variables->obj.pairs[i].key;
                buf_printf(out,"var [%s]\n", vname);
            }
        }
        if (lists) {
            for (int i=0;i<lists->obj.count;i++) {
                JVal *larr = lists->obj.pairs[i].val;
                const char *lname = (larr&&larr->type==JArr&&larr->arr.count>0&&larr->arr.items[0]->type==JStr)
                    ? larr->arr.items[0]->string : lists->obj.pairs[i].key;
                buf_printf(out,"list <%s>\n", lname);
            }
        }
        if ((variables&&variables->obj.count)||(lists&&lists->obj.count))
            buf_cat(out,"\n");

        int has_hats = 0;
        for (int i=0;i<blocks->obj.count;i++) {
            JVal *b = blocks->obj.pairs[i].val;
            JVal *tl = jobj_get(b,"topLevel");
            if (tl && tl->type==JBool && tl->boolean) { has_hats=1; break; }
        }
        if (!has_hats) return;
        buf_cat(out,"stage\n{\n");
        for (int i=0;i<blocks->obj.count;i++) {
            JVal *b = blocks->obj.pairs[i].val;
            JVal *tl = jobj_get(b,"topLevel");
            if (!tl||tl->type!=JBool||!tl->boolean) continue;
            decompile_hat(out,blocks,blocks->obj.pairs[i].key,1);
        }
        buf_cat(out,"}\n\n");
        return;
    }

    buf_printf(out,"sprite %s\n{\n", name);

    JVal *stage_vars  = stage_target ? jobj_get(stage_target,"variables") : NULL;
    JVal *stage_lists = stage_target ? jobj_get(stage_target,"lists")     : NULL;
    if (variables) {
        for (int i=0;i<variables->obj.count;i++) {
            JVal *varr = variables->obj.pairs[i].val;
            const char *vname = (varr&&varr->type==JArr&&varr->arr.count>0&&varr->arr.items[0]->type==JStr)
                ? varr->arr.items[0]->string : variables->obj.pairs[i].key;
            int is_global = 0;
            if (stage_vars)
                for (int j=0;j<stage_vars->obj.count;j++)
                    if (strcmp(stage_vars->obj.pairs[j].key,variables->obj.pairs[i].key)==0) { is_global=1; break; }
            if (!is_global) buf_printf(out,"    var [%s]\n", vname);
        }
    }
    if (lists) {
        for (int i=0;i<lists->obj.count;i++) {
            JVal *larr = lists->obj.pairs[i].val;
            const char *lname = (larr&&larr->type==JArr&&larr->arr.count>0&&larr->arr.items[0]->type==JStr)
                ? larr->arr.items[0]->string : lists->obj.pairs[i].key;
            int is_global = 0;
            if (stage_lists)
                for (int j=0;j<stage_lists->obj.count;j++)
                    if (strcmp(stage_lists->obj.pairs[j].key,lists->obj.pairs[i].key)==0) { is_global=1; break; }
            if (!is_global) buf_printf(out,"    list <%s>\n", lname);
        }
    }

    for (int i=0;i<blocks->obj.count;i++) {
        JVal *b  = blocks->obj.pairs[i].val;
        JVal *tl = jobj_get(b,"topLevel");
        if (!tl||tl->type!=JBool||!tl->boolean) continue;
        JVal *shadow = jobj_get(b,"shadow");
        if (shadow&&shadow->type==JBool&&shadow->boolean) continue;
        const char *bop = jstr(jobj_get(b,"opcode"));
        if (strcmp(bop,"procedures_definition")!=0) continue;
        JVal *binputs = jobj_get(b,"inputs");
        JVal *proto_inp = jobj_get(binputs,"custom_block");
        const char *proto_uid = jstr(jarr_get(proto_inp,1));
        JVal *proto = jobj_get(blocks,proto_uid);
        JVal *pmut  = proto ? jobj_get(proto,"mutation") : NULL;
        if (!pmut) continue;
        const char *proccode = jstr(jobj_get(pmut,"proccode"));
        int warp = 0;
        JVal *wv=jobj_get(pmut,"warp"); if(wv&&wv->type==JBool) warp=wv->boolean;
        char pname[256]; strncpy(pname,proccode,sizeof(pname)-1); pname[255]=0;
        char *pct=strstr(pname," %s"); if(pct)*pct='\0';
        const char *anames_str = jstr(jobj_get(pmut,"argumentnames"));
        buf_printf(out,"    custom block (%s)", pname);
        const char *q = anames_str;
        while (*q) {
            while (*q && *q != '"') q++;
            if (!*q) break;
            q++;
            const char *start = q;
            while (*q && *q != '"') q++;
            if (*q == '"') {
                int len = q-start < 63 ? q-start : 63;
                char param[64]; strncpy(param,start,len); param[len]=0;
                if (strlen(param)) buf_printf(out," (%s)", param);
                q++;
            }
        }
        if (warp) buf_cat(out," --no-refresh");
        buf_cat(out,"\n");
    }

    for (int i=0;i<blocks->obj.count;i++) {
        JVal *b  = blocks->obj.pairs[i].val;
        JVal *tl = jobj_get(b,"topLevel");
        if (!tl||tl->type!=JBool||!tl->boolean) continue;
        JVal *shadow = jobj_get(b,"shadow");
        if (shadow&&shadow->type==JBool&&shadow->boolean) continue;
        decompile_hat(out,blocks,blocks->obj.pairs[i].key,1);
    }

    buf_cat(out,"}\n\n");
}

/* ── main entry ─────────────────────────────────────────────────── */
int decompile_sb3(const char *sb3_path, const char *jappl_path) {
    int json_len=0;
    char *json = zip_read_file(sb3_path, "project.json", &json_len);
    if (!json) { fprintf(stderr,"cannot read project.json from %s\n",sb3_path); return -1; }

    JP jp; jp.s=json; jp.pos=0;
    JVal *root = jp_parse(&jp);
    free(json);

    JVal *targets = jobj_get(root,"targets");
    if (!targets||targets->type!=JArr) { fprintf(stderr,"no targets\n"); return -1; }

    Buf out; buf_init(&out);
    buf_cat(&out,"// decompiled by jappl2sb3\n\n");

    JVal *stage_target = NULL;
    for (int i=0;i<targets->arr.count;i++) {
        JVal *t = targets->arr.items[i];
        JVal *is = jobj_get(t,"isStage");
        if (is&&is->type==JBool&&is->boolean) { stage_target=t; break; }
    }

    if (stage_target)
        decompile_target(&out, stage_target, 1, NULL);

    for (int i=0;i<targets->arr.count;i++) {
        JVal *t = targets->arr.items[i];
        JVal *is = jobj_get(t,"isStage");
        if (is&&is->type==JBool&&is->boolean) continue;
        decompile_target(&out, t, 0, stage_target);
    }

    FILE *f = fopen(jappl_path,"w");
    if (!f) { fprintf(stderr,"cannot write %s\n",jappl_path); free(out.buf); return -1; }
    fwrite(out.buf,1,out.len,f);
    fclose(f);
    free(out.buf);
    return 0;
}
